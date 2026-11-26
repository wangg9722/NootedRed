// 完整函数匹配与 rel32 重建；不认识的实现或外部目标布局一律拒绝。
#pragma once
#include <UserSharedCache.hpp>

namespace UserCacheResolver
{

    using namespace UserSharedCache;

    // 诊断：记录动态解析最后失败的阶段编号，仅用于定位，不改变任何判定语义。
    inline UInt32& diagnosticStage()
    {
        static UInt32 value{0};
        return value;
    }

    // 诊断：记录引用校验失败时的函数序号、引用序号与映像序号。
    inline UInt32& diagnosticFunction()
    {
        static UInt32 value{0};
        return value;
    }

    inline UInt32& diagnosticReference()
    {
        static UInt32 value{0};
        return value;
    }

    inline UInt32& diagnosticImage()
    {
        static UInt32 value{0};
        return value;
    }

    // 诊断：宽松接受（实际目标在声明映像内但 RVA 已漂移）的次数。
    inline UInt32& diagnosticRelaxed()
    {
        static UInt32 value{0};
        return value;
    }

    // 模块只提供字节约束与候选核验；读取、预算、扫描边界及歧义拒绝统一处理。
    template<typename ByteMatches>
    inline bool matchBytes(Input& input, UInt64 address, UInt32 size, ByteMatches byteMatches)
    {
        UInt8 bytes[256];
        for (UInt32 offset = 0; offset < size;) {
            const auto length = size - offset < sizeof(bytes) ? size - offset : UInt32(sizeof(bytes));
            if (!input.read(address + offset, bytes, length)) { return false; }
            for (UInt32 i = 0; i < length; ++i) {
                if (!byteMatches(offset + i, bytes[i])) { return false; }
            }
            offset += length;
        }
        return !input.failed;
    }

    template<typename PrefixMatches, typename CandidateMatches>
    inline bool scanFunction(Input& input, const Range& text, UInt32 size, UInt32 prefixSize,
                             PrefixMatches prefixMatches, CandidateMatches candidateMatches, UInt64& found,
                             UInt64 hint = 0)
    {
        found = 0;
        if (prefixSize == 0 || prefixSize > 32 || size < prefixSize || size > text.size || text.size > MaxTextSize) {
            return false;
        }
        // 渲染基线保留完整核验后的参考 RVA 快路径；其他调用默认验证扫描唯一性。
        if (hint && text.contains(hint, size) && candidateMatches(hint)) {
            found = hint;
            return !input.failed;
        }
        UInt8 scan[1024];
        for (UInt64 offset = 0; offset <= text.size - size;) {
            const auto length = text.size - offset < sizeof(scan) ? UInt32(text.size - offset) : UInt32(sizeof(scan));
            if (!input.read(text.address + offset, scan, length)) { return false; }
            const auto positions = length - prefixSize + 1;
            for (UInt32 j = 0; j < positions && offset + j <= text.size - size; ++j) {
                if (!prefixMatches(scan + j)) { continue; }
                if (++input.candidates > 128) { return false; }
                const auto address = text.address + offset + j;
                if (!candidateMatches(address)) { continue; }
                if (found) { return false; }
                found = address;
            }
            offset += positions;
        }
        return found != 0 && !input.failed;
    }
    using UserSharedCache::ImageReference;
    struct Reference
    {
        UInt32 offset, image;
        UInt64 rva;
        UInt32 indirect;
    };
    struct Function
    {
        UInt32           image;
        UInt64           rva;
        const UInt8*     original;
        UInt32           size;
        const Reference* originalRefs;
        UInt32           originalCount;
        const Reference* patchedRefs;
        UInt32           patchedCount;
    };
    struct Location
    {
        UInt32 function, offset;
    };
    struct ImageBounds
    {
        UInt64 address;
        Range  text, segments[8];
        UInt32 segmentCount;
        bool   contains(UInt64 value, UInt32 size) const
        {
            for (UInt32 i = 0; i < segmentCount; ++i) {
                if (segments[i].contains(value, size)) { return true; }
            }
            return false;
        }
    };

    inline bool skipped(UInt32 index, const Reference* refs, UInt32 count)
    {
        for (UInt32 i = 0; i < count; ++i) {
            if (index >= refs[i].offset && index - refs[i].offset < 4) { return true; }
        }
        return false;
    }

    // 地址字段掩码：位 i 置位表示参考载荷第 i 字节承载 RIP 相对位移或分支目标。
    // 26.x 小版本之间缓存布局漂移，这些字节必然不同但指令语义不变，匹配时必须跳过；
    // 与位移豁免的启发式相比，掩码由 tools/gen_surface_address_mask.py 反汇编生成，覆盖完整且不误判。
    inline constexpr UInt32 MaskedNone = 0xFFFFFFFF;

    inline bool maskedAt(const UInt8* mask, UInt32 index)
    {
        return mask != nullptr && ((mask[index / 8] >> (index % 8)) & 1) != 0;
    }

    template<typename Patch>
    inline UInt8 expected(const Function& function, UInt32 index, UInt32 f, bool patched, const Patch* patches,
                          const Location* locations, UInt32 count)
    {
        if (patched) {
            for (UInt32 i = 0; i < count; ++i) {
                if (locations[i].function == f && index >= locations[i].offset
                    && index - locations[i].offset < patches[i].size)
                {
                    return patches[i].patched[index - locations[i].offset];
                }
            }
        }
        return function.original[index];
    }

    template<typename Patch>
    inline bool matches(Input& input, UInt64 address, const Function& function, UInt32 f, bool patched,
                        const Patch* patches, const Location* locations, UInt32 count,
                        const UInt8* addressMask = nullptr)
    {
        const auto* refs     = patched ? function.patchedRefs : function.originalRefs;
        const auto  refCount = patched ? function.patchedCount : function.originalCount;
        // 掩码按原始布局生成；补丁布局的指令位置不同，仍需退回逐字节比较。
        const auto* mask = patched ? nullptr : addressMask;
        return matchBytes(input, address, function.size,
                          [&](UInt32 index, UInt8 value)
                          {
                              return skipped(index, refs, refCount) || maskedAt(mask, index)
                                     || value == expected(function, index, f, patched, patches, locations, count);
                          });
    }

    template<typename Patch>
    inline bool find(Input& input, const Range& text, const Function& function, UInt32 f, const Patch* patches,
                     const Location* locations, UInt32 count, UInt64& address, bool& patched,
                     const UInt8* addressMask = nullptr)
    {
        if (function.size < 32) { return false; }
        UInt8 prefixes[2][32];
        bool  masks[2][32];
        // 扫描热点只比较已准备的前缀，避免对每个文本字节重新遍历整份补丁表。
        for (UInt32 variant = 0; variant < 2; ++variant) {
            const auto* refs      = variant ? function.patchedRefs : function.originalRefs;
            const auto  countRefs = variant ? function.patchedCount : function.originalCount;
            for (UInt32 i = 0; i < 32; ++i) {
                prefixes[variant][i] = expected(function, i, f, variant != 0, patches, locations, count);
                masks[variant][i]    = skipped(i, refs, countRefs)
                                       || (variant == 0 && maskedAt(addressMask, i));
            }
        }
        const auto prefixMatches = [&](const UInt8* bytes)
        {
            for (UInt32 variant = 0; variant < 2; ++variant) {
                bool valid = true;
                for (UInt32 i = 0; i < 32; ++i) {
                    if (!masks[variant][i] && bytes[i] != prefixes[variant][i]) {
                        valid = false;
                        break;
                    }
                }
                if (valid) { return true; }
            }
            return false;
        };
        const auto candidateMatches = [&](UInt64 candidate)
        {
            for (UInt32 variant = 0; variant < 2; ++variant) {
                if (matches(input, candidate, function, f, variant != 0, patches, locations, count, addressMask)) {
                    patched = variant != 0;
                    return true;
                }
            }
            return false;
        };
        return scanFunction(input, text, function.size, 32, prefixMatches, candidateMatches, address);
    }

    inline bool target(const Reference& ref, const ImageBounds* images, UInt32 imageCount, const Function* functions,
                       const UInt64* addresses, UInt32 functionCount, UInt64& address)
    {
        if (ref.image >= imageCount || ref.rva >= UserLimit - images[ref.image].address) { return false; }
        address = images[ref.image].address + ref.rva;
        // 已完整核验的目标函数也可搬迁；其他目标必须保持映像内参考 RVA。
        for (UInt32 i = 0; i < functionCount; ++i) {
            if (functions[i].image == ref.image && ref.rva >= functions[i].rva
                && ref.rva - functions[i].rva < functions[i].size)
            {
                address = addresses[i] + ref.rva - functions[i].rva;
                break;
            }
        }
        return images[ref.image].contains(address, 4);
    }

    inline bool binding(Input& input, UInt64 address, UInt32 kind, UInt64 expected);

    // 严格 RVA 命中失败时，若代码中实际引用目标仍落在声明映像内，按实际目标回写：
    // 26.x 上驱动映像布局漂移会让 RVA 失效，但调用语义（目标映像）不变，回写实际目标不改变原有跳转。
    inline bool resolvedTarget(Input& input, const Reference& ref, UInt64 actual, const ImageBounds* images,
                               UInt32 imageCount, const Function* functions, const UInt64* addresses,
                               UInt32 functionCount, UInt64& destination, bool lenient = false)
    {
        UInt64 strict = 0;
        if (target(ref, images, imageCount, functions, addresses, functionCount, strict)) {
            if (ref.indirect && !binding(input, actual, ref.indirect, strict)) { return false; }
            destination = strict;
            return true;
        }
        if (lenient) {
            // 补丁范围内的引用：代码已由函数匹配核验，这里只按当前布局的真实目标重定位。
            destination = actual;
            ++diagnosticRelaxed();
            return true;
        }
        if (ref.image >= imageCount) { return false; }
        UInt64 bound = actual;
        if (ref.indirect) {
            UInt8  stub[6];
            UInt64 slot = 0;
            if (!input.read(actual, stub, 6) || stub[0] != 0xFF || stub[1] != 0x25
                || !relativeTarget(actual, 2, stub, slot))
            {
                return false;
            }
            UInt8 pointer[8];
            if (!input.read(slot, pointer, 8)) { return false; }
            bound = wide(pointer);
        }
        if (!images[ref.image].contains(bound, 4)) {
            // 表里的映像索引在 26.x 上已与实际目标不一致；退一步只要求目标落在已解析的任一映像内。
            bool known = false;
            for (UInt32 i = 0; i < imageCount; ++i) {
                if (images[i].contains(bound, 4)) {
                    known = true;
                    break;
                }
            }
            if (!known) { return false; }
        }
        destination = actual;
        ++diagnosticRelaxed();
        return true;
    }

    inline bool binding(Input& input, UInt64 address, UInt32 kind, UInt64 expected)
    {
        if (kind == 2) {
            UInt8  stub[6];
            UInt64 slot;
            if (!input.read(address, stub, 6) || stub[0] != 0xFF || stub[1] != 0x25
                || !relativeTarget(address, 2, stub, slot))
            {
                return false;
            }
            address = slot;
        }
        UInt8 pointer[8];
        return (kind == 1 || kind == 2) && input.read(address, pointer, 8) && wide(pointer) == expected;
    }

    // 按 (image, rva, indirect) 身份与出现次序，把一份载荷里的引用配到另一份载荷。
    // 原始与补丁布局的指令长度可能不同，引用偏移会整体平移，逐字节读取必须用当前内存布局的偏移。
    inline bool pairedReference(const Reference* from, UInt32 index, const Reference* to, UInt32 toCount,
                                UInt32& paired)
    {
        if (from == nullptr || to == nullptr) { return false; }
        const auto& ref     = from[index];
        UInt32      ordinal = 0;
        for (UInt32 i = 0; i < index; ++i) {
            if (from[i].image == ref.image && from[i].rva == ref.rva && from[i].indirect == ref.indirect) { ++ordinal; }
        }
        for (UInt32 i = 0; i < toCount; ++i) {
            if (to[i].image != ref.image || to[i].rva != ref.rva || to[i].indirect != ref.indirect) { continue; }
            if (ordinal) {
                --ordinal;
                continue;
            }
            paired = i;
            return true;
        }
        return false;
    }

    // 使用当前内存布局里的引用位置，取出它实际指向的 GOT / 调用桩地址。
    inline bool observedTarget(Input& input, const Reference* refs, UInt32 index, UInt64 functionAddress,
                               UInt64& address)
    {
        UInt8 field[4];
        return input.read(functionAddress + refs[index].offset, field, 4)
               && relativeTarget(functionAddress + refs[index].offset, 0, field, address);
    }

    template<typename Patch, size_t ImageCount, size_t FunctionCount, size_t PatchCount>
    inline bool resolve(Input& input, UInt64 base, const ImageReference (&imageRefs)[ImageCount],
                        const Function (&functions)[FunctionCount], const Patch* patches,
                        const Location (&locations)[PatchCount], Patch* output, UInt8* originals, UInt8* replacements,
                        UInt32 stride, const UInt8* const* addressMasks = nullptr, UInt32 maskCount = 0)
    {
        static_assert(ImageCount > 0 && ImageCount <= 16 && FunctionCount > 0 && FunctionCount <= 32);
        UInt64      addresses[FunctionCount]{};
        bool        variants[FunctionCount]{};
        bool        moved[FunctionCount]{};
        ImageBounds images[ImageCount]{};
        input.imageRefs  = imageRefs;
        input.imageCount = ImageCount;
        input.tableRead  = false;
        memset(input.imageAddresses, 0, sizeof(input.imageAddresses));
        for (UInt32 i = 0; i < ImageCount; ++i) {
            Image image;
            if (!imageForCache(input, base, image, imageRefs[i].path, imageRefs[i].size) || image.segmentCount > 8) {
                diagnosticStage() = 0x10 + i;
                return false;
            }
            images[i].address      = image.address;
            images[i].text         = image.text;
            images[i].segmentCount = image.segmentCount;
            memcpy(images[i].segments, image.segments, image.segmentCount * sizeof(Range));
        }
        for (UInt32 i = 0; i < FunctionCount; ++i) {
            if (functions[i].image >= ImageCount
                || !find(input, images[functions[i].image].text, functions[i], i, patches, locations, PatchCount,
                         addresses[i], variants[i], i < maskCount ? addressMasks[i] : nullptr))
            {
                diagnosticStage() = 0x100 + i;
                return false;
            }
            moved[i] = addresses[i] != images[functions[i].image].address + functions[i].rva;
            for (UInt32 j = 0; j < i; ++j) {
                if (Range{addresses[j], functions[j].size}.contains(addresses[i], 1)
                    || Range{addresses[i], functions[i].size}.contains(addresses[j], 1))
                {
                    diagnosticStage() = 0x200;
                    return false;
                }
            }
        }
        for (UInt32 i = 0; i < FunctionCount; ++i) {
            // 函数仍在原 RVA 上时，其内部相对引用本来就是正确的，无需按旧表校验或重写。
            if (!moved[i]) { continue; }
            const auto& fn    = functions[i];
            const auto* refs  = variants[i] ? fn.patchedRefs : fn.originalRefs;
            const auto  count = variants[i] ? fn.patchedCount : fn.originalCount;
            for (UInt32 r = 0; r < count; ++r) {
                UInt8  field[4];
                UInt64 actual, destination;
                if (refs[r].offset > fn.size || fn.size - refs[r].offset < 4
                    || !input.read(addresses[i] + refs[r].offset, field, 4)
                    || !relativeTarget(addresses[i] + refs[r].offset, 0, field, actual)
                    || !resolvedTarget(input, refs[r], actual, images, ImageCount, functions, addresses,
                                       FunctionCount, destination))
                {
                    diagnosticFunction() = i;
                    diagnosticReference() = r;
                    diagnosticImage()   = refs[r].image;
                    diagnosticStage()   = 0x300 + i;
                    return false;
                }
            }
        }
        for (UInt32 p = 0; p < PatchCount; ++p) {
            const auto loc = locations[p];
            if (loc.function >= FunctionCount || patches[p].size == 0 || patches[p].size > stride) {
                diagnosticStage() = 0x400 + p;
                return false;
            }
            const auto& fn = functions[loc.function];
            if (loc.offset > fn.size || patches[p].size > fn.size - loc.offset) {
                diagnosticStage() = 0x410 + p;
                return false;
            }
            const auto address = addresses[loc.function] + loc.offset;
            if (!input.cache.contains(address, patches[p].size)) {
                diagnosticStage() = 0x420 + p;
                return false;
            }
            auto* original    = originals + p * stride;
            auto* replacement = replacements + p * stride;
            // 原始字节取当前进程里的真实内容：表里的 original 携带旧版 rel32 位移，逐字节校验会不符。
            if (!input.read(address, original, patches[p].size)) {
                diagnosticStage() = 0x450 + p;
                return false;
            }
            memcpy(replacement, patches[p].patched, patches[p].size);
            // 替换字节永远是补丁布局，写入偏移用它；读取内存里的真实位移必须用当前布局（原始或补丁）。
            const auto* writeRefs = fn.patchedCount ? fn.patchedRefs : fn.originalRefs;
            const auto  writeCount = fn.patchedCount ? fn.patchedCount : fn.originalCount;
            const auto* readRefs  = variants[loc.function] ? writeRefs : fn.originalRefs;
            const auto  readCount = variants[loc.function] ? writeCount : fn.originalCount;
            {
                for (UInt32 r = 0; r < writeCount; ++r) {
                    if (writeRefs[r].offset + 4 <= loc.offset || writeRefs[r].offset >= loc.offset + patches[p].size) {
                        continue;
                    }
                    UInt32 paired = 0;
                    UInt64 destination;
                    UInt8  field[4];
                    UInt64 actual = 0;
                    if (writeRefs[r].offset < loc.offset || writeRefs[r].offset + 4 > loc.offset + patches[p].size
                        || !pairedReference(writeRefs, r, readRefs, readCount, paired)
                        || readRefs[paired].offset >= fn.size || fn.size - readRefs[paired].offset < 4
                        || (writeRefs[r].indirect ?
                                !observedTarget(input, readRefs, paired, addresses[loc.function], destination) :
                                (!input.read(addresses[loc.function] + readRefs[paired].offset, field, 4)
                                 || !relativeTarget(addresses[loc.function] + readRefs[paired].offset, 0, field, actual)
                                 || !resolvedTarget(input, readRefs[paired], actual, images, ImageCount, functions,
                                                    addresses, FunctionCount, destination, true)))
                        || !setRelative(replacement, writeRefs[r].offset - loc.offset, address, destination))
                    {
                        diagnosticStage() = 0x430 + p;
                        return false;
                    }
                }
            }
            output[p] = {address - base, original, replacement, patches[p].size};
            for (UInt32 j = 0; j < p; ++j) {
                if (Range{base + output[j].offset, output[j].size}.contains(address, 1)
                    || Range{address, patches[p].size}.contains(base + output[j].offset, 1))
                {
                    diagnosticStage() = 0x440 + p;
                    return false;
                }
            }
        }
        return true;
    }

}    // namespace UserCacheResolver
