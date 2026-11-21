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
    // 诊断：记录本解析器最后失败的步骤编号。
    inline UInt32& diagnosticReason()
    {
        static UInt32 value{0};
        return value;
    }

    struct Plan
    {
        // 0 init / 1 override / 2 blend gate / 3 blend allow / 4 scratch
        UserSurfaceSyncData::Patch patches[5]{};
        UInt8                      original[5][MaxPatchSize]{};
        UInt8                      patched[5][MaxPatchSize]{};
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
                                                        || (blend
                                                            && ((index >= BlendGateInConstructor
                                                                    && index - BlendGateInConstructor < 2)
                                                                || (index >= BlendAllowInConstructor
                                                                    && index - BlendAllowInConstructor < 3)))
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

    inline bool resolve(Reader reader, UInt64 base, const DevicePatchSet& device, bool shared, bool blend, Plan& plan,
                        bool scratchMode = false)
    {
        Input input{reader, {}};
        Image image;
        if (!imageForCache(input, base, image)) {
            diagnosticReason() = 1;
            return false;
        }
        // Scratch 与撤回的 VT 策略按完整函数和实际相对引用核验。
        if (scratchMode) {
            using namespace ScratchResolverData;
            if (!UserCacheResolver::resolve(input, base, ResolverImages, ResolverFunctions, device.scratch + 3,
                                            ResolverLocations, plan.patches + 4, &plan.original[4][0],
                                            &plan.patched[4][0], MaxPatchSize))
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
        // Scratch 函数只属于 -NRedScratch 策略；非 scratch 模式下它的 26.0 形状在 26.7.x 已不成立，
        // 曾经因为在这里强制匹配而让整条 resolver 直接放弃（NRedImmediateSyncDynReason=32），
        // 连带 LegacyBlend 的门也一条都没打上。
        if (scratchMode
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
        memcpy(plan.original[3], BlendAllowOriginal, sizeof(BlendAllowOriginal));
        memcpy(plan.patched[3], BlendAllowPatched, sizeof(BlendAllowPatched));
        const UInt64 addresses[] = {init, overrideAddress, constructor + BlendGateInConstructor,
                                    constructor + BlendAllowInConstructor};
        for (UInt32 i = 0; i < 4; ++i) {
            const auto length = i == 0   ? sizeof(Original0)
                                : i == 1 ? sizeof(Original1)
                                : i == 2 ? sizeof(BlendGateOriginal)
                                         : sizeof(BlendAllowOriginal);
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
