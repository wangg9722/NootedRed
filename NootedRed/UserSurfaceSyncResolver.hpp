// 按驱动路径与完整函数内容解析兼容的共享缓存；仅生成计划，不写入进程。
#pragma once
#include <UserSharedCache.hpp>
#include <UserSurfaceSyncData.hpp>
#include <string.h>

namespace UserSurfaceSyncResolver
{

    using namespace UserSurfaceSyncData;
    using UserSharedCache::Image;
    using UserSharedCache::Input;
    using UserSharedCache::MaxTextSize;
    using UserSharedCache::Range;
    using UserSharedCache::Reader;
    using UserSharedCache::relativeTarget;
    using UserSharedCache::setRelative;
    using UserSharedCache::UserLimit;
    using UserSharedCache::word;
    using UserSharedCache::wide;
    // 诊断：记录本解析器最后失败的步骤编号。
    inline UInt32& diagnosticReason()
    {
        static UInt32 value{0};
        return value;
    }

    struct Plan
    {
        UserSurfaceSyncData::Patch patches[4]{};
        UInt8                      original[4][MaxPatchSize]{};
        UInt8                      patched[4][MaxPatchSize]{};
        UInt32                     reads{0};
    };
    inline bool imageForCache(Input& input, UInt64 base, Image& image)
    {
        return UserSharedCache::imageForCache(input, base, image, DriverPath, sizeof(DriverPath))
               && image.constantCount != 0 && image.text.size <= MaxTextSize;
    }


    // 模板里含 rel32 位移；不同系统构建上这些字节会变，比较时按指令形态跳过。
    inline bool displacementByte(const UInt8* expected, UInt32 index, UInt32 size)
    {
        const UInt32 from = index > 3 ? index - 3 : 0;
        for (UInt32 pos = from; pos <= index; ++pos) {
            const UInt8 b = expected[pos];
            if ((b == 0xE8 || b == 0xE9) && index - pos >= 1 && index - pos <= 4) { return true; }
            if (pos + 2 < size && (b == 0x48 || b == 0x4C)) {
                const UInt8 op = expected[pos + 1], modrm = expected[pos + 2];
                if ((op == 0x8D || op == 0x8B || op == 0x89 || op == 0x39 || op == 0x3B)
                    && (modrm & 0xC7) == 0x05 && index > pos + 2 && index - (pos + 2) <= 4)
                {
                    return true;
                }
            }
        }
        return false;
    }


    // 大函数在 26.x 上可能有极少量非位移字节差异；按差异预算比较，仍要求前 32 字节严格一致。
    inline bool similar(Input& input, UInt64 address, const UInt8* expected, UInt32 size, UInt32 maxDiff)
    {
        UInt8  chunk[256];
        UInt32 diff = 0;
        for (UInt32 offset = 0; offset < size;) {
            const UInt32 length = size - offset < 256 ? size - offset : 256;
            if (!input.read(address + offset, chunk, length)) { return false; }
            for (UInt32 i = 0; i < length; ++i) {
                const UInt32 index = offset + i;
                if (chunk[i] == expected[index] || displacementByte(expected, index, size)) { continue; }
                if (++diff > maxDiff) { return false; }
            }
            offset += length;
        }
        return !input.failed;
    }

    inline bool skipped(UInt32 index, const RelativeField* fields, UInt32 fieldCount)
    {
        for (UInt32 i = 0; i < fieldCount; ++i) {
            if (index >= fields[i].offset && index - fields[i].offset < 4) { return true; }
        }
        return false;
    }

    inline bool matches(Input& input, UInt64 address, const UInt8* expected, UInt32 size,
                        const RelativeField* fields = nullptr, UInt32 fieldCount = 0, bool blend = false)
    {
        return UserCacheResolver::matchBytes(input, address, size,
                                             [&](UInt32 index, UInt8 value)
                                             {
                                                 return skipped(index, fields, fieldCount)
                                                        || (!fields && displacementByte(expected, index, size))
                                                        || (blend && index >= BlendGateInConstructor
                                                            && index - BlendGateInConstructor < 2)
                                                        || value == expected[index];
                                             });
    }

    inline bool findFunction(Input& input, const Range& text, const UInt8* expected, UInt32 size, UInt64& found,
                             bool blend = false, UInt64 hint = 0, UInt32 diffBudget = 0)
    {
        if (size < 32) { return false; }
        return UserCacheResolver::scanFunction(
            input, text, size, 32,
            [&](const UInt8* bytes)
            {
                for (UInt32 i = 0; i < 32; ++i) {
                    if (bytes[i] != expected[i] && !displacementByte(expected, i, size)) { return false; }
                }
                return true;
            },
            [&](UInt64 address)
            {
                if (diffBudget && similar(input, address, expected, size, diffBudget)) { return true; }
                return matches(input, address, expected, size, nullptr, 0, blend);
            },
            found, hint);
    }

    // 定点写 8 字节指针的扫描；按 8 字节步进，块之间留一次重叠。
    inline UInt64 pointerIn(Input& input, const Range& range, UInt64 value, UInt64 skip)
    {
        UInt8 chunk[4096];
        for (UInt64 offset = 0; offset + 8 <= range.size; offset += sizeof(chunk) - 8) {
            const UInt32 length = range.size - offset < sizeof(chunk) ? static_cast<UInt32>(range.size - offset)
                                                                       : static_cast<UInt32>(sizeof(chunk));
            if (!input.read(range.address + offset, chunk, length)) { return 0; }
            for (UInt32 i = 0; i + 8 <= length; i += 8) {
                if (wide(chunk + i) == value && range.address + offset + i != skip) {
                    return range.address + offset + i;
                }
            }
            if (length < sizeof(chunk)) { break; }
        }
        return 0;
    }

    // 只在字节范围内找唯一出现；用于选择器字符串与能力分支。
    inline UInt64 uniqueBytes(Input& input, const Range& range, const UInt8* bytes, UInt32 size, UInt64& second)
    {
        UInt8  chunk[4096];
        UInt64 first{0};
        second = 0;
        for (UInt64 offset = 0; offset + size <= range.size; offset += sizeof(chunk) - size) {
            const UInt32 length = range.size - offset < sizeof(chunk) ? static_cast<UInt32>(range.size - offset)
                                                                       : static_cast<UInt32>(sizeof(chunk));
            if (!input.read(range.address + offset, chunk, length)) { return 0; }
            for (UInt32 i = 0; i + size <= length; ++i) {
                if (memcmp(chunk + i, bytes, size) != 0) { continue; }
                const UInt64 address = range.address + offset + i;
                if (first == 0) { first = address; }
                else if (address != first) { second = address; return 0; }
            }
            if (length < sizeof(chunk)) { break; }
        }
        return first;
    }

    // 通过 dyld 符号表按方法名定位渲染管线构造函数；不依赖任何一版机器码模板。
    inline bool constructorFromSelector(Input& input, const Image& image, UInt64& constructor)
    {
        static const char name[] =
            "-[GFX9_MtlRenderPipelineState initWithDevice:pipelineStateDescriptor:vertexVariant:fragmentVariant:]";
        UInt64 symbol{0};
        if (!UserSharedCache::symbolAddress(input, image, name, sizeof(name) - 1, symbol)) {
            diagnosticReason() = 34;
            return false;
        }
        constructor = symbol + image.slide;
        if (!image.text.contains(constructor, 1)) {
            diagnosticReason() = 35;
            return false;
        }
        return true;
    }

    // 只在构造函数体内找唯一的能力分支；命中后写 2 字节 NOP，其余字节一律不动。
    inline const UInt8* patchedGate()
    {
        static const UInt8 bytes[2] = {0x90, 0x90};
        return bytes;
    }

    inline bool resolveGate(Reader reader, UInt64 base, UInt8* original, UserSurfaceSyncData::Patch& patch)
    {
        Input input{reader, {}};
        static const char metalPath[] =
            "/System/Library/Extensions/AMDRadeonX5000MTLDriver.bundle/Contents/MacOS/AMDRadeonX5000MTLDriver";
        static const UserSharedCache::ImageReference refs[1] = {{metalPath, sizeof(metalPath) - 1}};
        input.imageRefs  = refs;
        input.imageCount = 1;
        input.tableRead  = false;
        Image image;
        if (!imageForCache(input, base, image, metalPath, sizeof(metalPath) - 1)) {
            diagnosticReason() = 30 + UserSharedCache::imageReason();
            return false;
        }
        // 26.7.3 上该闸门位于 image 内固定 RVA；用 16 字节签名做校验，错版就不会落补丁。
        constexpr UInt64 GateRva = 0x29708;
        static const UInt8 GateSignature[16] = {0x02, 0x48, 0x8b, 0x9d, 0x70, 0xfe, 0xff, 0xff,
                                                0x74, 0x28, 0x80, 0xbd, 0x80, 0xfe, 0xff, 0xff};
        const auto         gateAddress = image.address + GateRva;
        UInt8              probe[16];
        if (!image.text.contains(gateAddress, 2) || !input.read(gateAddress - 8, probe, sizeof(probe))
            || memcmp(probe, GateSignature, sizeof(probe)) != 0)
        {
            diagnosticReason() = 37;
            return false;
        }
        original[0] = 0x74;
        original[1] = 0x28;
        patch = {gateAddress - base, original, patchedGate(), 2};
        return true;
    }

    inline bool resolve(Reader reader, UInt64 base, const DevicePatchSet& device, bool shared, bool blend, Plan& plan,
                        bool scratchMode = false)
    {
        Input input{reader, {}};
        static const char metalPath[] =
            "/System/Library/Extensions/AMDRadeonX5000MTLDriver.bundle/Contents/MacOS/AMDRadeonX5000MTLDriver";
        static const UserSharedCache::ImageReference refs[1] = {{metalPath, sizeof(metalPath) - 1}};
        input.imageRefs  = refs;
        input.imageCount = 1;
        input.tableRead  = false;
        Image image;
        if (!imageForCache(input, base, image, metalPath, sizeof(metalPath) - 1)) {
            diagnosticReason() = 30 + UserSharedCache::imageReason();
            return false;
        }
        // Scratch 与撤回的 VT 策略按完整函数和实际相对引用核验。
        if (scratchMode) {
            using namespace ScratchResolverData;
            if (!UserCacheResolver::resolve(input, base, ResolverImages, ResolverFunctions, device.scratch + 3,
                                            ResolverLocations, plan.patches + 3, &plan.original[3][0],
                                            &plan.patched[3][0], MaxPatchSize))
            {
                diagnosticReason() = 2;
                return false;
            }
        }
        UInt64 settings{0}, scratch{0}, constructor{0};
        if (!findFunction(input, image.text, NativeOverride, sizeof(NativeOverride), settings, false,
                          image.address + NativeOverrideRVA))
        {
            diagnosticReason() = 31;
            return false;
        }
        if (!scratchMode
            && !findFunction(input, image.text, ScratchOriginal, sizeof(ScratchOriginal), scratch, false,
                             image.address + ScratchRVA))
        {
            diagnosticReason() = 32;
            return false;
        }
        if (!findFunction(input, image.text, BlendConstructor, sizeof(BlendConstructor), constructor, true,
                          image.address + ConstructorRVA, 96))
        {
            diagnosticReason() = 33;
            return false;
        }
        const UInt8*         variants[] = {Original0, device.immediate[0].patched, device.shared[0].patched};
        const RelativeField* links[]    = {OriginalInitLinks, ImmediateInitLinks, SharedInitLinks};
        UInt64               init{0}, targets[3]{};
        const auto           matchInitAt = [&](UInt64 address, UInt64* result)
        {
            if (!image.text.contains(address, sizeof(Original0))) { return false; }
            UInt8 initBytes[sizeof(Original0)];
            for (UInt32 variant = 0; variant < 3; ++variant) {
                if (!matches(input, address, variants[variant], sizeof(Original0), links[variant], 3)
                    || !input.read(address, initBytes, sizeof(initBytes)))
                {
                    continue;
                }
                UInt64 candidateTargets[3]{};
                bool   valid = true;
                for (UInt32 k = 0; k < 3; ++k) {
                    valid &= relativeTarget(address, links[variant][k].offset, initBytes,
                                            candidateTargets[links[variant][k].target]);
                }
                UInt8 constant[4];
                valid = valid && candidateTargets[1] == settings && image.constant(candidateTargets[0])
                        && input.read(candidateTargets[0], constant, sizeof(constant)) && word(constant) == 0x40400000
                        && image.text.contains(candidateTargets[2], sizeof(NativeDump))
                        && matches(input, candidateTargets[2], NativeDump, sizeof(NativeDump));
                if (valid) {
                    memcpy(result, candidateTargets, sizeof(candidateTargets));
                    return true;
                }
            }
            return false;
        };
        if (!UserCacheResolver::scanFunction(
                input, image.text, sizeof(Original0), 32,
                [&](const UInt8* bytes)
                {
                    for (auto* variant : variants) {
                        if (memcmp(bytes, variant, 32) == 0) { return true; }
                    }
                    return false;
                },
                [&](UInt64 address) { return matchInitAt(address, targets); }, init, image.address + InitRVA))
        {
            diagnosticReason() = 4;
            return false;
        }
        UInt64     overrideAddress{0};
        const auto matchOverrideAt = [&](UInt64 address)
        {
            UInt8 bytes[sizeof(Original1)];
            if (!image.text.contains(address, sizeof(bytes)) || !input.read(address, bytes, sizeof(bytes))) {
                return false;
            }
            const bool original = memcmp(bytes, Original1, 6) == 0;
            const bool patched  = bytes[0] == 0xE9 && memcmp(bytes + 5, Patched1 + 5, 5) == 0;
            UInt64     target{0};
            return (original || patched) && relativeTarget(address, original ? 6 : 1, bytes, target)
                   && target == (original ? settings : init + OverrideEntryInInit);
        };
        if (!UserCacheResolver::scanFunction(
                input, image.text, sizeof(Original1), sizeof(Original1),
                [&](const UInt8* bytes)
                {
                    return memcmp(bytes, Original1, 6) == 0
                           || (bytes[0] == 0xE9 && memcmp(bytes + 5, Patched1 + 5, 5) == 0);
                },
                matchOverrideAt, overrideAddress, image.address + OverrideRVA))
        {
            diagnosticReason() = 5;
            return false;
        }
        const auto* source =
            scratchMode ? device.scratch : (blend ? device.blend : (shared ? device.shared : device.immediate));
        const auto* patchedLinks = shared || blend ? SharedInitLinks : ImmediateInitLinks;
        memcpy(plan.original[0], Original0, sizeof(Original0));
        memcpy(plan.patched[0], source[0].patched, sizeof(Original0));
        for (UInt32 k = 0; k < 3; ++k) {
            if (!setRelative(plan.original[0], OriginalInitLinks[k].offset, init, targets[OriginalInitLinks[k].target])
                || !setRelative(plan.patched[0], patchedLinks[k].offset, init, targets[patchedLinks[k].target]))
            {
                diagnosticReason() = 6;
                return false;
            }
        }
        memcpy(plan.original[1], Original1, sizeof(Original1));
        memcpy(plan.patched[1], Patched1, sizeof(Patched1));
        if (!setRelative(plan.original[1], 6, overrideAddress, settings)
            || !setRelative(plan.patched[1], 1, overrideAddress, init + OverrideEntryInInit))
        {
            diagnosticReason() = 7;
            return false;
        }
        memcpy(plan.original[2], BlendGateOriginal, sizeof(BlendGateOriginal));
        memcpy(plan.patched[2], BlendGatePatched, sizeof(BlendGatePatched));
        const UInt64 addresses[] = {init, overrideAddress, constructor + BlendGateInConstructor};
        for (UInt32 i = 0; i < 3; ++i) {
            const auto length = i == 0 ? sizeof(Original0) : (i == 1 ? sizeof(Original1) : sizeof(BlendGateOriginal));
            if (!input.cache.contains(addresses[i], length)) {
                diagnosticReason() = 8;
                return false;
            }
            plan.patches[i] = {addresses[i] - base, plan.original[i], plan.patched[i], static_cast<UInt32>(length)};
        }
        plan.reads = input.reads;
        return true;
    }

}    // namespace UserSurfaceSyncResolver
