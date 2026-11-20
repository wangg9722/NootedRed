"""生成经过版本与机器码核对的 Metal 修复数据，不写系统缓存。"""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
from metal_cache import CacheReader, macho, array, IMAGE
from build_compute_scratch_patch import generate as generate_scratch, supported_devices
from cache_resolver_data import ResolverData, containing_function

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/user-surface-sync"
INIT_SYMBOL = "__Z29amdMtl_HWL_InitDeviceSettingsP18AMD_DeviceSettingsP18AMDCommonHwInfoRec"
OVERRIDE_SYMBOL = "__Z33amdMtl_HWL_OverrideDeviceSettingsP18AMD_DeviceSettingsP18AMDCommonHwInfoRec"
CONSTRUCTOR_SYMBOL = ("-[GFX9_MtlRenderPipelineState initWithDevice:pipelineStateDescriptor:"
                      "vertexVariant:fragmentVariant:]")
# 原生配置入口与调试桩不再是固定地址：它们由初始化函数自己的跳转给出，
# 这样同一份生成器可以跟着每一个 macOS 版本的镜像布局走。
OVERRIDE_PROLOGUE = bytes.fromhex("554889e55350")
DUMP_CODE = bytes.fromhex("554889e55dc3")


def external_jumps(code, address):
    # 只接受 e8/e9 rel32；初始化函数对原生配置入口的调用是整个数据集的锚点。
    for offset in range(len(code) - 4):
        if code[offset] in (0xE8, 0xE9):
            target = address + offset + 5 + struct.unpack_from("<i", code, offset + 1)[0]
            yield offset, target


def find_native_entries(cache, init_address, init_code):
    override = dump = None
    for _, target in external_jumps(init_code, init_address):
        try:
            head = cache.read(target, 16)
        except ValueError:
            continue
        if head.startswith(OVERRIDE_PROLOGUE):
            override = target
        elif head[:len(DUMP_CODE)] == DUMP_CODE:
            dump = target
    if override is None:
        raise ValueError("原生配置入口未找到")
    if dump is None:
        raise ValueError("原生调试桩未找到")
    return override, cache.read(override, 923), dump


def float_constant(cache, address, code, value):
    # 渲染侧常量按 RIP 相对引用在函数体内定位，不再写死镜像绝对地址。
    hits = []
    for offset in range(len(code) - 4):
        target = address + offset + 4 + struct.unpack_from("<i", code, offset)[0]
        try:
            if cache.read(target, 4) == struct.pack("<f", value):
                hits.append(target)
        except ValueError:
            continue
    if len(hits) != 1:
        raise ValueError(f"原生常量不唯一：{len(hits)}")
    return hits[0]


def for_device(records, device_id):
    # 完整 32 位比较只作用于本机核显，保留同进程中的独显原生初始化。
    guard = bytes.fromhex("817e0c38160000")
    payload = bytes.fromhex(records[0]["patched"])
    if payload.count(guard) != 1:
        raise ValueError("初始化设备比较指令未核实")
    result = [dict(record) for record in records]
    result[0]["patched"] = payload.replace(guard, guard[:3] + struct.pack("<I", device_id)).hex()
    return result


def relocation_fields(cache, code, address, targets, label):
    # 渲染专属目标编号保留；指令／数据分界与 rel32 解码复用公共生成器。
    resolver = ResolverData(cache, WORK)
    fields = []
    for offset, image, rva, indirect in resolver.references(code, address, label):
        target = resolver.images[image][1]+rva
        if indirect or target not in targets:
            raise ValueError(f'未核实的渲染重定位目标：{target:#x}')
        fields.append((offset, targets.index(target)))
    if sorted(kind for _, kind in fields) != [0, 1, 2]:
        raise ValueError('初始化外部引用不完整')
    return fields


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    cache = CacheReader()
    try:
        originals = {}
        for label, name in (("init", INIT_SYMBOL), ("override", OVERRIDE_SYMBOL)):
            address, code = cache.function(name)
            originals[label] = {"address": address, "code": code.hex()}
        if len(bytes.fromhex(originals["init"]["code"])) != 215:
            raise ValueError("初始化函数长度变化")
        native_override, native_override_code, native_dump = find_native_entries(
            cache, originals["init"]["address"], bytes.fromhex(originals["init"]["code"]))
        constant = float_constant(cache, originals["init"]["address"],
                                  bytes.fromhex(originals["init"]["code"]), 3.0)
        assembly = (ROOT / "tools/surface_sync_init.s").read_text()
        for token, value in (("@INIT@", originals["init"]["address"]), ("@CONST@", constant),
                             ("@OVERRIDE@", native_override), ("@DUMP@", native_dump)):
            assembly = assembly.replace(token, hex(value))
        if any(token in assembly for token in ("@INIT@", "@CONST@", "@OVERRIDE@", "@DUMP@")):
            raise ValueError("初始化汇编锚点未替换")
        obj = WORK / "init.o"
        source = WORK / "init.s"
        source.write_text(assembly)
        subprocess.run(["clang", "-c", str(source), "-o", str(obj)], check=True, timeout=60)
        data = obj.read_bytes()
        _, sections, _ = macho(lambda a, n: data[a:a+n], 0)
        text = next(s for s in sections if s[0] == "__text")
        payload = data[text[3]:text[3]+text[2]]
        if len(payload) != 215:
            raise ValueError("初始化补丁长度错误")
        records = []
        for label in originals:
            item = originals[label]
            patched = payload if label == "init" else b"\xe9" + struct.pack(
                "<i", originals["init"]["address"] + 166 - item["address"] - 5) + b"\x90" * 5
            if len(patched) != len(bytes.fromhex(item["code"])):
                raise ValueError("补丁越界")
            records.append({"label": label, "address": item["address"],
                            "original": item["code"], "patched": patched.hex()})
        # 单独生成 shared SRD 策略，保留只禁用延迟同步的兼容模式。
        candidate_obj = WORK / "srd-shared-init.o"
        subprocess.run(["clang", "-c", str(ROOT / "tools/surface_sync_srd_shared_init.s"),
                        "-o", str(candidate_obj)], check=True, timeout=60)
        candidate_data = candidate_obj.read_bytes()
        _, candidate_sections, _ = macho(lambda a, n: candidate_data[a:a+n], 0)
        candidate_text = next(s for s in candidate_sections if s[0] == "__text")
        candidate_payload = candidate_data[candidate_text[3]:candidate_text[3]+candidate_text[2]]
        if len(candidate_payload) != 215:
            raise ValueError("共享 SRD 的补丁长度错误")
        candidate_records = [dict(r) for r in records]
        candidate_records[0]["patched"] = candidate_payload.hex()
        device_records = [{"device_id": device, "records": for_device(records, device),
                           "srd_shared_records": for_device(candidate_records, device)}
                          for device in supported_devices()]
        ctor_address, ctor_code = cache.function(CONSTRUCTOR_SYMBOL)
        if len(ctor_code) < 1000:
            raise ValueError("pipeline 构造函数未核实")
        # 能力分支在构造函数里按唯一性定位，版本之间偏移变化不影响生成。
        gates = [offset for offset in range(len(ctor_code) - 1)
                 if ctor_code[offset:offset + 2] == bytes.fromhex("7428")]
        if len(gates) != 1:
            raise ValueError(f"RB+ 能力分支不唯一：{len(gates)}")
        gate_address = ctor_address + gates[0]
        gate_code = ctor_code[gates[0]:gates[0] + 2]
        legacy_records = [dict(r) for r in candidate_records]
        legacy_records.append({"label": "blend-capability-gate", "address": gate_address,
                               "original": gate_code.hex(), "patched": "9090"})
        # 仅保留旧实验的原始字节检查；不生成已撤回的 VT 替换代码。
        # 撤回实验的原始分支只做旁证；系统版本改写了这些位置时直接不带入数据。
        withdrawn_guards = []
        for address, expected in [(0x7FF81585FD97, "7428"), (0x7FF815861D17, "0f8482040000")]:
            try:
                original = cache.read(address, len(bytes.fromhex(expected)))
            except ValueError:
                continue
            if original == bytes.fromhex(expected):
                withdrawn_guards.append({"address": address, "original": original.hex()})
        device_scratch_records = [
            {"device_id": device, "records": [*legacy_records, generate_scratch(cache, device)]}
            for device in supported_devices()
        ]
        scratch_record = device_scratch_records[0]["records"][-1]
        scratch_records = device_scratch_records[0]["records"]
        pages = {r["address"] & ~4095 for r in scratch_records}
        for r in scratch_records:
            if r["address"] // 4096 != (r["address"] + len(bytes.fromhex(r["original"])) - 1) // 4096:
                raise ValueError("单段跨页")
        header = "// 由 tools/build_surface_sync_patch.py 生成，不手工编辑。\n#pragma once\n#include <IOKit/IOTypes.h>\n#include <UserCacheResolver.hpp>\n"
        header += "namespace UserSurfaceSyncData {\n" + array("CacheMagic", cache.magic) + array("CacheUUID", cache.uuid)
        header += f"inline constexpr UInt64 CacheBase = 0x{cache.base:X}ULL;\n"
        header += "struct Patch { UInt64 offset; const UInt8* original; const UInt8* patched; UInt32 size; };\n"
        for i, r in enumerate(records):
            header += array(f"Original{i}", bytes.fromhex(r["original"])) + array(f"Patched{i}", bytes.fromhex(r["patched"]))
            header += array(f"SrdSharedPatched{i}", bytes.fromhex(candidate_records[i]["patched"]))

        header += "inline constexpr Patch Patches[] = {\n" + ",\n".join(
            f"    {{0x{r['address']-cache.base:X}ULL, Original{i}, Patched{i}, sizeof(Original{i})}}"
            for i, r in enumerate(records)) + "\n};\n"
        header += "inline constexpr Patch SrdSharedPatches[] = {\n" + ",\n".join(
            f"    {{0x{r['address']-cache.base:X}ULL, Original{i}, SrdSharedPatched{i}, sizeof(Original{i})}}"
            for i, r in enumerate(records)) + "\n};\n"
        header += array("BlendGateOriginal", gate_code) + array("BlendGatePatched", bytes.fromhex("9090"))
        header += "inline constexpr Patch LegacyBlendPatches[] = {\n" + ",\n".join(
            f"    {{0x{r['address']-cache.base:X}ULL, " +
            (f"Original{i}, SrdSharedPatched{i}, sizeof(Original{i})" if i < 2 else
             "BlendGateOriginal, BlendGatePatched, sizeof(BlendGateOriginal)") + "}"
            for i, r in enumerate(legacy_records)) + "\n};\n"
        header += "struct Guard { UInt64 offset; const UInt8* original; UInt32 size; };\n"
        for i, r in enumerate(withdrawn_guards):
            header += array(f"WithdrawnOriginal{i}", bytes.fromhex(r["original"]))
        header += "inline constexpr Guard WithdrawnTransferGuards[] = {\n" + ",\n".join(
            f"    {{0x{r['address']-cache.base:X}ULL, WithdrawnOriginal{i}, sizeof(WithdrawnOriginal{i})}}"
            for i, r in enumerate(withdrawn_guards)) + "\n};\n"
        header += array("ScratchOriginal", bytes.fromhex(scratch_record["original"]))
        # 各设备共享原始代码、覆盖跳转与混合分支；仅初始化比较立即数不同。
        for entry in device_records[1:]:
            suffix = f"{entry['device_id']:04X}"
            for mode, key in [("Immediate", "records"), ("Shared", "srd_shared_records")]:
                header += array(f"{mode}Init{suffix}", bytes.fromhex(entry[key][0]["patched"]))
                header += f"inline constexpr Patch {mode}Patches{suffix}[] = {{\n"
                header += (f"    {{Patches[0].offset, Original0, {mode}Init{suffix}, sizeof(Original0)}},\n"
                           f"    Patches[1]\n}};\n")
            header += f"inline constexpr Patch BlendPatches{suffix}[] = {{\n"
            header += (f"    SharedPatches{suffix}[0], SharedPatches{suffix}[1], LegacyBlendPatches[2]\n}};\n")
        for entry in device_scratch_records:
            device = entry["device_id"]
            suffix = f"{device:04X}"
            patched_name = "ScratchPatched" if device == 0x1638 else f"ScratchPatched{suffix}"
            patch_name = "ComputeScratchPatches" if device == 0x1638 else f"ComputeScratchPatches{suffix}"
            blend_name = "LegacyBlendPatches" if device == 0x1638 else f"BlendPatches{suffix}"
            record = entry["records"][-1]
            header += array(patched_name, bytes.fromhex(record["patched"]))
            header += f"inline constexpr Patch {patch_name}[] = {{\n"
            header += ",\n".join(f"    {blend_name}[{i}]" for i in range(len(legacy_records))) + ",\n"
            header += (f"    {{0x{record['address']-cache.base:X}ULL, ScratchOriginal, {patched_name}, "
                       "sizeof(ScratchOriginal)}\n};\n")
        header += ("struct DevicePatchSet { UInt32 deviceID; const Patch* immediate; const Patch* shared; "
                   "const Patch* blend; const Patch* scratch; };\n")
        header += "inline constexpr DevicePatchSet DevicePatches[] = {\n"
        header += "    {0x1638, Patches, SrdSharedPatches, LegacyBlendPatches, ComputeScratchPatches},\n"
        header += ",\n".join(f"    {{0x{e['device_id']:04X}, ImmediatePatches{e['device_id']:04X}, "
                             f"SharedPatches{e['device_id']:04X}, BlendPatches{e['device_id']:04X}, "
                             f"ComputeScratchPatches{e['device_id']:04X}}}"
                             for e in device_records[1:]) + "\n};\n"
        header += ("inline constexpr const DevicePatchSet* findDevice(UInt32 deviceID) {\n"
                   "    for (const auto& entry : DevicePatches) { if (entry.deviceID == deviceID) { return &entry; } }\n"
                   "    return nullptr;\n}\n")
        # 动态缓存解析只接受相同实现的完整函数；UUID 不参与兼容判定。
        targets = [constant, native_override, native_dump]
        init_address = originals['init']['address']
        variants = [('OriginalInitLinks', bytes.fromhex(records[0]['original'])),
                    ('ImmediateInitLinks', payload), ('SharedInitLinks', candidate_payload)]
        header += 'struct RelativeField { UInt32 offset; UInt32 target; };\n'
        for name, code in variants:
            fields = relocation_fields(cache, code, init_address, targets, name)
            header += f'inline constexpr RelativeField {name}[] = {{\n'
            header += ',\n'.join(f'    {{{offset}, {kind}}}' for offset, kind in fields) + '\n};\n'
        if not native_override_code.startswith(OVERRIDE_PROLOGUE) or len(native_override_code) != 923:
            raise ValueError('原生配置函数未核实')
        header += array('NativeOverride', native_override_code)
        header += array('NativeDump', DUMP_CODE)
        header += array('BlendConstructor', ctor_code)
        header += f'inline constexpr UInt32 BlendGateInConstructor = {gate_address - ctor_address};\n'
        header += f'inline constexpr UInt32 OverrideEntryInInit = 166;\n'
        header += f'inline constexpr char DriverPath[] = "{IMAGE}";\n'
        for name, address in [('InitRVA', init_address), ('OverrideRVA', originals['override']['address']),
                              ('NativeOverrideRVA', targets[1]), ('ConstructorRVA', ctor_address),
                              ('ScratchRVA', scratch_record['address'])]:
            header += f'inline constexpr UInt64 {name} = 0x{address-cache.image_base:X}ULL;\n'
        scratch_resolver = ResolverData(cache, WORK / 'scratch')
        scratch_resolver.add(scratch_record['address'], bytes.fromhex(scratch_record['original']),
                             bytes.fromhex(scratch_record['patched']))
        # 撤回策略的 VT 跳转仍需按完整原生函数核验，不能沿用参考缓存的固定偏移。
        if len(withdrawn_guards) == 2:
            _, vt_base = scratch_resolver.owner(withdrawn_guards[0]['address'])
            vt_address, vt_code = containing_function(cache, vt_base, withdrawn_guards[0]['address'])
            if not vt_address <= withdrawn_guards[1]['address'] < vt_address + len(vt_code):
                raise ValueError('撤回跳转的原生函数范围变化')
            scratch_resolver.add(vt_address, vt_code, vt_code)
        header += 'namespace ScratchResolverData {\n' + scratch_resolver.header([(0, 0)]) + '}\n'
        header += f"inline constexpr UInt32 MaxPatchSize = 452;\ninline constexpr UInt32 PageCount = {len(pages)};\n}}\n"
        (WORK / "UserSurfaceSyncData.hpp").write_text(header)
        (ROOT / "NootedRed/UserSurfaceSyncData.hpp").write_text(header)
        (WORK / "patch-data.json").write_text(json.dumps({"originals": originals, "records": records,
                                                         "srd_shared_records": candidate_records,
                                                         "device_records": device_records,
                                                         "legacy_blend_records": legacy_records,
                                                         "withdrawn_guards": withdrawn_guards,
                                                         "compute_scratch_records": scratch_records,
                                                         "device_compute_scratch_records": device_scratch_records,
                                                         "cache_base": cache.base}, indent=2) + "\n")
        print(f"Generated render patches for {len(device_records)} NRed PCI IDs / "
              f"{len(device_scratch_records)} device-specific scratch patches; at most {len(pages)} pages")
    finally:
        cache.close()


if __name__ == "__main__":
    main()
