// 有读取预算的 dyld 缓存与 Mach-O 解析；仅读取当前进程，供用户态补丁共用。
#pragma once
#include <IOKit/IOTypes.h>
#include <string.h>
namespace UserSharedCache
{

    using Reader                   = int (*)(UInt64, void*, size_t);
    constexpr UInt64   UserLimit   = 0x0000800000000000ULL;
    constexpr UInt32   MaxTextSize = 8 * 1024 * 1024;
    struct ImageReference
    {
        const char* path;
        UInt32      size;
    };

    inline UInt32& imageReason()
    {
        static UInt32 value{0};
        return value;
    }

    struct Range
    {
        UInt64 address{0}, size{0};
        bool   contains(UInt64 value, UInt64 length) const
        {
            return value >= address && value - address <= size && length <= size - (value - address);
        }
    };

    struct Input
    {
        Reader                reader;
        Range                 cache;
        UInt32                reads{0}, bytes{0}, candidates{0};
        bool                  failed{false}, tableRead{false};
        const ImageReference* imageRefs{nullptr};
        UInt32                imageCount{0};
        UInt64                imageAddresses[16]{};
        bool                  read(UInt64 address, void* value, UInt32 size)
        {
            if (failed || !cache.contains(address, size) || reads >= 32768 || size > 32 * 1024 * 1024 - bytes) {
                failed = true;
                return false;
            }
            ++reads;
            bytes += size;
            if (reader(address, value, size) != 0) {
                failed = true;
                return false;
            }
            return true;
        }
    };

    inline UInt32 word(const UInt8* value)
    {
        UInt32 result;
        memcpy(&result, value, sizeof(result));
        return result;
    }

    inline UInt64 wide(const UInt8* value)
    {
        UInt64 result;
        memcpy(&result, value, sizeof(result));
        return result;
    }

    inline bool relativeTarget(UInt64 address, UInt32 offset, const UInt8* bytes, UInt64& target)
    {
        if (bytes == nullptr || address >= UserLimit - 4 || offset >= UserLimit - address - 4) { return false; }
        int displacement;
        memcpy(&displacement, bytes + offset, sizeof(displacement));
        const auto next = address + offset + 4;
        if (displacement < 0) {
            const auto magnitude = static_cast<UInt64>(-static_cast<long long>(displacement));
            if (next < magnitude) { return false; }
            target = next - magnitude;
        }
        else {
            if (next >= UserLimit || static_cast<UInt64>(displacement) >= UserLimit - next) { return false; }
            target = next + static_cast<UInt64>(displacement);
        }
        return true;
    }

    inline bool setRelative(UInt8* bytes, UInt32 offset, UInt64 address, UInt64 target)
    {
        if (bytes == nullptr || address >= UserLimit - 4 || offset >= UserLimit - address - 4) { return false; }
        const auto next = address + offset + 4;
        if (next >= UserLimit || target >= UserLimit) { return false; }
        const auto magnitude = target >= next ? target - next : next - target;
        if (magnitude > (target >= next ? 0x7FFFFFFFULL : 0x80000000ULL)) { return false; }
        const auto signedValue =
            target >= next ? static_cast<long long>(magnitude) : -static_cast<long long>(magnitude);
        const auto value = static_cast<int>(signedValue);
        memcpy(bytes + offset, &value, sizeof(value));
        return true;
    }

    struct Image
    {
        UInt64 address{0};
        Range  text;
        Range  constants[8]{};
        UInt32 constantCount{0};
        Range  segments[16]{};
        UInt32 segmentCount{0};
        // 符号表：LC_SYMTAB 与 __LINKEDIT 的映射关系，用于按名字定位函数。
        UInt64 linkeditAddress{0}, linkeditSize{0}, linkeditOffset{0};
        UInt64 symbolTableAddress{0}, symbolCount{0}, stringTableAddress{0}, stringTableSize{0};
        UInt64 pendingSymbolOffset{0}, pendingStringOffset{0};
        UInt64 slide{0};
        bool   contains(UInt64 value, UInt32 size) const
        {
            for (UInt32 i = 0; i < segmentCount; ++i) {
                if (segments[i].contains(value, size)) { return true; }
            }
            return false;
        }
        bool constant(UInt64 value) const
        {
            for (UInt32 i = 0; i < constantCount; ++i) {
                if (constants[i].contains(value, 4)) { return true; }
            }
            return false;
        }
    };

    inline bool imageForCache(Input& input, UInt64 base, Image& image, const char* pathName, UInt32 pathSize)
    {
        UInt8 header[456];
        imageReason() = 0;
        if (!pathName || pathSize == 0 || pathSize > 256 || !input.reader || base >= UserLimit - sizeof(header)
            || (base & 4095) != 0 || input.reader(base, header, sizeof(header)) != 0
            || memcmp(header, "dyld_v1 x86_64h", 16) != 0)
        {
            imageReason() = 1;
            return false;
        }
        // Darwin 25 的现代 image table；所有元数据都必须落在主缓存的首个只读映射内。
        const auto mappingOffset = word(header + 16), mappingCount = word(header + 20);
        const auto unslid = wide(header + 224), size = wide(header + 232), maxSlide = wide(header + 240);
        if (mappingOffset < sizeof(header) || mappingOffset > 65536 || mappingCount == 0 || mappingCount > 64
            || unslid == 0 || unslid > base || base - unslid > maxSlide || size == 0 || size > 0x1000000000ULL
            || size > UserLimit - base)
        {
            imageReason() = 2;
            return false;
        }
        input.cache = {base, size};
        UInt8 mapping[32];
        if (!input.read(base + mappingOffset, mapping, sizeof(mapping)) || wide(mapping) != unslid
            || wide(mapping + 16) != 0 || (word(mapping + 28) & 1) == 0)
        {
            imageReason() = 1; return false;
        }
        const auto metadataSize = wide(mapping + 8);
        if (metadataSize == 0 || metadataSize > size) { imageReason() = 2; return false; }
        const Range metadata{base, metadataSize < 16 * 1024 * 1024 ? metadataSize : 16 * 1024 * 1024};
        const auto  imagesOffset = word(header + 448), imageCount = word(header + 452);
        if (imageCount == 0 || imageCount > 8192 || imagesOffset < sizeof(header)
            || !metadata.contains(base + imagesOffset, static_cast<UInt64>(imageCount) * 32))
        {
            imageReason() = 3; return false;
        }
        const auto slide = base - unslid;
        UInt64     imageAddress{0};
        UInt32     pathReadSize = pathSize;
        if (input.imageCount > 16) { imageReason() = 4; return false; }
        for (UInt32 p = 0; p < input.imageCount; ++p) {
            if (!input.imageRefs[p].path || input.imageRefs[p].size > 256 || input.imageRefs[p].size == 0) {
                imageReason() = 5; return false;
            }
            if (pathReadSize < input.imageRefs[p].size) { pathReadSize = input.imageRefs[p].size; }
        }
        UInt8 entries[512];
        for (UInt32 i = 0; !input.tableRead && i < imageCount;) {
            const auto count = imageCount - i < 16 ? imageCount - i : 16;
            if (!input.read(base + imagesOffset + static_cast<UInt64>(i) * 32, entries, count * 32)) { imageReason() = 6; return false; }
            for (UInt32 j = 0; j < count; ++j) {
                const auto* entry      = entries + j * 32;
                const auto  pathOffset = word(entry + 24);
                if (!metadata.contains(base + pathOffset, pathReadSize)) { imageReason() = 7; return false; }
                char path[256];
                if (!input.read(base + pathOffset, path, pathReadSize)) { imageReason() = 8; return false; }
                for (UInt32 p = 0; p < input.imageCount; ++p) {
                    if (memcmp(path, input.imageRefs[p].path, input.imageRefs[p].size) != 0) { continue; }
                    const auto candidate = wide(entry);
                    if (input.imageAddresses[p] || candidate < unslid || candidate - unslid >= size) { imageReason() = 9; return false; }
                    input.imageAddresses[p] = candidate + slide;
                }
                if (memcmp(path, pathName, pathSize) != 0) { continue; }
                const auto address = wide(entry);
                if (imageAddress != 0 || address < unslid || address - unslid >= size) { imageReason() = 10; return false; }
                imageAddress = address + slide;
            }
            i += count;
        }
        if (input.imageCount) {
            input.tableRead = true;
            for (UInt32 p = 0; p < input.imageCount; ++p) {
                if (!input.imageAddresses[p]) { imageReason() = 11; return false; }
                if (input.imageRefs[p].size == pathSize && memcmp(input.imageRefs[p].path, pathName, pathSize) == 0) {
                    imageAddress = input.imageAddresses[p];
                }
            }
        }
        if (imageAddress == 0) { imageReason() = 4; return false; }
        image.address = imageAddress;
        image.slide   = slide;
        UInt8 mach[32];
        if (!input.read(imageAddress, mach, sizeof(mach)) || word(mach) != 0xFEEDFACF || word(mach + 4) != 0x01000007
            || (word(mach + 12) != 6 && word(mach + 12) != 8))
        {
            imageReason() = 5;
            return false;
        }
        const auto commands = word(mach + 16), commandSize = word(mach + 20);
        if (commands == 0 || commands > 128 || commandSize > 65536
            || !input.cache.contains(imageAddress + 32, commandSize))
        {
            imageReason() = 12; return false;
        }
        UInt32 cursor{0};
        for (UInt32 i = 0; i < commands; ++i) {
            UInt8 command[72];
            if (cursor > commandSize || commandSize - cursor < 8 || !input.read(imageAddress + 32 + cursor, command, 8))
            {
                imageReason() = 13; return false;
            }
            const auto kind = word(command), length = word(command + 4);
            if (length < 8 || (length & 7) != 0 || length > commandSize - cursor) { imageReason() = 14; return false; }
            if (kind == 0x19) {
                if (length < sizeof(command) || !input.read(imageAddress + 32 + cursor, command, sizeof(command))) {
                    imageReason() = 15; return false;
                }
                const auto sections = word(command + 64);
                if (sections > 64 || length != 72 + sections * 80) { imageReason() = 16; return false; }
                const auto segmentStart = wide(command + 24), segmentSize = wide(command + 32);
                if (segmentStart < unslid || segmentStart - unslid >= size
                    || segmentSize > size - (segmentStart - unslid))
                {
                    imageReason() = 17; return false;
                }
                const Range segment{segmentStart + slide, segmentSize};
                UInt8 segmentName[20];
                if (!input.read(imageAddress + 32 + cursor + 8, segmentName, 17)) { imageReason() = 18; return false; }
                if (memcmp(segmentName, "__LINKEDIT", 11) == 0) {
                    image.linkeditAddress = segment.address;
                    image.linkeditSize  = segment.size;
                    image.linkeditOffset = wide(command + 40);
                }
                if (image.segmentCount == 16) { imageReason() = 19; return false; }
                image.segments[image.segmentCount++] = segment;
                for (UInt32 j = 0; j < sections; ++j) {
                    UInt8 section[80];
                    if (!input.read(imageAddress + 32 + cursor + 72 + j * 80, section, sizeof(section))) {
                        imageReason() = 20; return false;
                    }
                    const auto address = wide(section + 32), sectionSize = wide(section + 40);
                    if (address < unslid || address - unslid >= size || !segment.contains(address + slide, sectionSize))
                    {
                        imageReason() = 21; return false;
                    }
                    if (memcmp(section, "__text\0", 7) == 0 && (word(command + 60) & 5) == 5) {
                        if (image.text.size != 0 || sectionSize == 0 || sectionSize > 64 * 1024 * 1024) {
                            imageReason() = 22; return false;
                        }
                        image.text = {address + slide, sectionSize};
                    }
                    if (memcmp(section, "__const\0", 8) == 0 && (word(command + 60) & 1) != 0) {
                        if (image.constantCount == 8) { imageReason() = 23; return false; }
                        image.constants[image.constantCount++] = {address + slide, sectionSize};
                    }
                }
            }
            if (kind == 0x2) {
                // 之前只读了 8 字节的 cmd/cmdsize，必须补读完整命令再取字段。
                if (length < 24) { imageReason() = 41; return false; }
                if (!input.read(imageAddress + 32 + cursor, command, 24)) { imageReason() = 42; return false; }
                // LC_SYMTAB：cmd/cmdsize 之后 symoff/nsyms/stroff/strsize 全是 32 位。
                const UInt32 symbolOffset = word(command + 8), symbolTotal = word(command + 12);
                const UInt32 stringOffset = word(command + 16), stringSize  = word(command + 20);
                if (symbolTotal == 0 || symbolTotal > 2000000 || stringSize == 0 || stringSize > 128 * 1024 * 1024)
                {
                    imageReason() = 24; return false;
                }
                image.symbolCount   = symbolTotal;
                image.stringTableSize = stringSize;
                image.pendingSymbolOffset = symbolOffset;
                image.pendingStringOffset = stringOffset;
            }
            cursor += length;
        }
        // dyld 共享缓存的 LC_SYMTAB 偏移是缓存级的，不能按单 image 的 __LINKEDIT 长度校验。
        // 这里只记录，缺失符号表不影响按固定 RVA + 字节签名定位。
        if (image.linkeditSize != 0 && image.symbolCount != 0
            && image.pendingSymbolOffset + image.symbolCount * 16 <= image.linkeditSize)
        {
            image.symbolTableAddress = image.linkeditAddress + image.pendingSymbolOffset - image.linkeditOffset;
            image.stringTableAddress = image.linkeditAddress + image.pendingStringOffset - image.linkeditOffset;
        }
        if (cursor != commandSize) { imageReason() = 27; return false; }
        if (image.text.size == 0) { imageReason() = 28; return false; }
        return true;
    }

    // 下一个符号地址即函数尾部边界；不读字符串表，只扫 nlist。
    inline bool symbolNext(Input& input, const Image& image, UInt64 address, UInt64& next)
    {
        UInt8        entries[512];
        UInt64       best{0};
        for (UInt64 base = 0; base < image.symbolCount; base += sizeof(entries) / 16) {
            const UInt64 left  = image.symbolCount - base;
            const UInt32 count = left < sizeof(entries) / 16 ? static_cast<UInt32>(left) : sizeof(entries) / 16;
            if (!input.read(image.symbolTableAddress + base * 16, entries, count * 16)) { return false; }
            for (UInt32 i = 0; i < count; ++i) {
                const UInt64 value = wide(entries + i * 16 + 8);
                if (value > address && (best == 0 || value < best)) { best = value; }
            }
        }
        if (best == 0) { return false; }
        next = best + image.slide;
        return true;
    }

    // 按符号名在 __LINKEDIT 符号表里定位函数；分块读取，避免逐项读爆预算。
    inline bool symbolAddress(Input& input, const Image& image, const char* name, UInt32 nameSize, UInt64& address)
    {
        if (image.symbolCount == 0 || nameSize == 0 || nameSize > 128) { return false; }
        UInt8  entries[512];
        UInt32 found{0};
        for (UInt64 base = 0; base < image.symbolCount; base += sizeof(entries) / 16) {
            const UInt64 left = image.symbolCount - base;
            const UInt32 count = left < sizeof(entries) / 16 ? static_cast<UInt32>(left) : sizeof(entries) / 16;
            if (!input.read(image.symbolTableAddress + base * 16, entries, count * 16)) { return false; }
            for (UInt32 i = 0; i < count; ++i) {
                const UInt32 stringIndex = word(entries + i * 16 + 4);
                const UInt64 value       = wide(entries + i * 16 + 8);
                if (stringIndex == 0 || stringIndex >= image.stringTableSize || value == 0) { continue; }
                UInt8 head[128];
                const auto length = image.stringTableSize - stringIndex;
                const UInt32 probe = length < nameSize + 1 ? static_cast<UInt32>(length) : nameSize + 1;
                if (!input.read(image.stringTableAddress + stringIndex, head, probe)) { return false; }
                if (probe < nameSize + 1 || memcmp(head, name, nameSize) != 0 || head[nameSize] != 0) { continue; }
                if (++found > 1) { return false; }
                address = value;
            }
        }
        return found == 1;
    }

}    // namespace UserSharedCache
