// 在进程私有代码页应用已核验的视频解码补丁，并保留原生调用与页面保护。
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserLegacyData.hpp>
#include <UserPatchMemory.hpp>
#include <UserPatchPolicy.hpp>
#include <UserVideoDecode.hpp>
#include <UserDecoderGuardData.hpp>
#include <UserVideoDecodeData.hpp>
#include <UserVideoDecodeResolver.hpp>
#include <VCN.hpp>
#include <VCNCapabilities.hpp>
#include <kern/task.h>
#include <mach/i386/vm_param.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <sys/proc.h>

namespace
{

    struct SharedRegionCheckArgs
    {
        user_addr_t startAddress;
    };
    static_assert(sizeof(SharedRegionCheckArgs) == 8);
    using SharedRegionCheck = int       (*)(proc_t, SharedRegionCheckArgs*, int*);
    using GetTaskMap        = vm_map_t         (*)(task_t);
    using RegionRecurse     = UserPatchMemory::RegionQuery;
    using CopyIn            = int                  (*)(user_addr_t, void*, size_t);
    using CopyOut           = int                 (*)(const void*, user_addr_t, size_t);

    mach_vm_address_t                 originalCheck{0};
    GetTaskMap                        getTaskMap{nullptr};
    RegionRecurse                     queryRegion{nullptr};
    CopyIn                            readUser{nullptr};
    CopyOut                           writeUser{nullptr};
    UInt32                            seenStages{0}, appliedCount{0}, errorCount{0};
    bool                              fairPlayDriverOnly{false};
    bool                              decodeGuard{true};
    bool                              legacyNoVCNUser{false}, legacyNoFairPlay{false}, legacyNoGuard{false};
    UInt32                            decodeGuardApplied{0}, decodeGuardRejected{0};
    const UserVideoDecodeData::Patch* devicePatches{nullptr};
    const UserLegacyCompat::Profile*  legacyProfile{nullptr};
    constexpr auto                    PatchCount   = arrsize(UserVideoDecodeData::Patches);
    constexpr UInt32                  MaxPageCount = PatchCount * 2;
    static_assert(PatchCount <= 32 && UserVideoDecodeData::PageCount <= 16);

    enum class Stage : UInt32
    {
        Called,
        OriginalError,
        Argument,
        CacheRead,
        CacheMismatch,
        CodeRead,
        CodeMismatch,
        Already,
        Mixed,
        NoMap,
        Region,
        Protection,
        Write,
        Restore,
        Rollback,
        Applied
    };

    void stage(const Stage value, const UInt32 fairPlayPID = 0)
    {
        static const char* const keys[] = {"NRedVCNUserD00", "NRedVCNUserD01", "NRedVCNUserD02", "NRedVCNUserD03",
                                           "NRedVCNUserD04", "NRedVCNUserD05", "NRedVCNUserD06", "NRedVCNUserD07",
                                           "NRedVCNUserD08", "NRedVCNUserD09", "NRedVCNUserD10", "NRedVCNUserD11",
                                           "NRedVCNUserD12", "NRedVCNUserD13", "NRedVCNUserD14", "NRedVCNUserD15"};
        const auto               index  = static_cast<UInt32>(value);
        if (fairPlayPID != 0) {
            NRed::singleton().setProp32("NRedDRMVideoLastPID", fairPlayPID);
            NRed::singleton().setProp32("NRedDRMVideoLastStage", index);
        }
        if ((__atomic_fetch_or(&seenStages, 1U << index, __ATOMIC_RELAXED) & (1U << index)) == 0) {
            NRed::singleton().setProp32(keys[index], 1);
            if (value == Stage::Applied) { NRed::singleton().setProp32("NRedVCNGVASelectionFix", 1); }
            SYSLOG("UserVideoDecode", "stage=%u", index);
        }
    }

    void error(const kern_return_t value)
    {
        auto count = __atomic_load_n(&errorCount, __ATOMIC_RELAXED);
        while (count < 8) {
            if (__atomic_compare_exchange_n(&errorCount, &count, count + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            {
                NRed::singleton().setProp32("NRedVCNUserLastError", static_cast<UInt32>(value));
                SYSLOG("UserVideoDecode", "rejected: error=0x%X", value);
                break;
            }
        }
    }

    using UserPatchMemory::Page;

    bool leafProtection(vm_map_t map, const mach_vm_address_t page, vm_prot_t& protection)
    {
        return UserPatchMemory::leafProtection(map, page, protection, queryRegion, error);
    }

    bool restorePages(vm_map_t map, Page* pages, const UInt32 count)
    {
        return UserPatchMemory::restorePages(map, pages, count, error);
    }

    // 在进程内分配一页可执行守卫，把 AMDRadeonVADriver2 的 VADecoder::Init 入口改为绝对跳转。
    // 守卫按会话规格拒绝超规格 HEVC 硬解（宽 > 4096 或 高 > 2304），让播放器回退软解，
    // VCN 因此收不到超出自身能力的提交。入口 24 字节由守卫原样重放后跳回。
    kern_return_t installDecodeGuard(vm_map_t map, const UInt64 base, bool& fresh)
    {
        using namespace UserDecoderGuardData;
        fresh = false;
        if (base >= UserSharedCache::UserLimit) { return KERN_INVALID_ADDRESS; }

        static constexpr UInt8 patchedPrefix[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        // 各版本缓存里 VADecoder::Init 的偏移不同：只认入口 24 字节与原函数一致的那一个候选。
        UInt64 entry{0};
        UInt8  current[PatchSize];
        for (UInt32 i = 0; i < OffsetCount; ++i) {
            const UInt64 offset = Offsets[i];
            if (offset >= UserSharedCache::UserLimit - base || PatchSize > UserSharedCache::UserLimit - base - offset) {
                continue;
            }
            const UInt64 candidate = base + offset;
            UInt8        bytes[PatchSize];
            if (readUser(candidate, bytes, sizeof(bytes)) != 0) { continue; }
            if (memcmp(bytes, EntryOriginal, sizeof(bytes)) == 0) {
                entry = candidate;
                memcpy(current, bytes, sizeof(current));
                NRed::singleton().setProp32("NRedDecodeGuardOffsetIndex", i);
                break;
            }
            if (memcmp(bytes, patchedPrefix, sizeof(patchedPrefix)) == 0) {
                UInt64 target{0};
                memcpy(&target, bytes + 6, sizeof(target));
                if (target == 0 || (target & (PAGE_SIZE - 1)) != 0
                    || target >= UserSharedCache::UserLimit - sizeof(Payload))
                {
                    continue;
                }
                UInt8 existing[sizeof(Payload)], expected[sizeof(Payload)];
                memcpy(expected, Payload, sizeof(expected));
                const UInt64 continuation = candidate + PatchSize;
                memcpy(expected + PayloadJumpOffset, &continuation, sizeof(continuation));
                vm_prot_t protection{VM_PROT_NONE};
                if (readUser(target, existing, sizeof(existing)) == 0 && memcmp(existing, expected, sizeof(existing)) == 0
                    && leafProtection(map, target, protection) && protection == (VM_PROT_READ | VM_PROT_EXECUTE))
                {
                    NRed::singleton().setProp32("NRedDecodeGuardOffsetIndex", i);
                    return KERN_SUCCESS;
                }
            }
        }
        if (entry == 0) { return KERN_INVALID_ARGUMENT; }

        const UInt64 entryPage = entry & ~static_cast<UInt64>(PAGE_SIZE - 1);
        const UInt64 continuation = entry + PatchSize;
        vm_prot_t    protection{VM_PROT_NONE};
        if ((entry & (PAGE_SIZE - 1)) + sizeof(current) > PAGE_SIZE || !leafProtection(map, entryPage, protection)
            || protection != (VM_PROT_READ | VM_PROT_EXECUTE))
        {
            return KERN_PROTECTION_FAILURE;
        }

        vm_address_t page{0};
        auto         result = vm_allocate(map, &page, PAGE_SIZE, VM_FLAGS_ANYWHERE);
        if (result != KERN_SUCCESS) { return result; }

        UInt8 payload[sizeof(Payload)];
        memcpy(payload, Payload, sizeof(payload));
        memcpy(payload + PayloadJumpOffset, &continuation, sizeof(continuation));
        if (writeUser(payload, page, sizeof(payload)) != 0) {
            vm_deallocate(map, page, PAGE_SIZE);
            return KERN_FAILURE;
        }
        result = vm_protect(map, page, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        if (result == KERN_SUCCESS) { result = vm_protect(map, page, PAGE_SIZE, TRUE, VM_PROT_READ | VM_PROT_EXECUTE); }
        if (result != KERN_SUCCESS) {
            vm_deallocate(map, page, PAGE_SIZE);
            return result;
        }

        // FF 25 00 00 00 00 <绝对目标>：14 字节，其余补 NOP。
        UInt8 patch[PatchSize];
        memset(patch, 0x90, sizeof(patch));
        memcpy(patch, patchedPrefix, sizeof(patchedPrefix));
        const UInt64 target = page;
        memcpy(patch + 6, &target, sizeof(target));

        result = vm_protect(map, entryPage, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (result != KERN_SUCCESS) {
            vm_deallocate(map, page, PAGE_SIZE);
            return result;
        }
        const auto written    = writeUser(patch, entry, sizeof(patch));
        bool       rolledBack = false;
        if (written != 0) { rolledBack = writeUser(current, entry, sizeof(current)) == 0; }
        auto restored = vm_protect(map, entryPage, PAGE_SIZE, FALSE, protection);
        if (restored == KERN_SUCCESS) { restored = vm_protect(map, entryPage, PAGE_SIZE, TRUE, protection); }
        if (written != 0 || restored != KERN_SUCCESS) {
            if (rolledBack) { vm_deallocate(map, page, PAGE_SIZE); }
            return written != 0 ? KERN_FAILURE : restored;
        }
        fresh = true;
        NRed::singleton().setProp32("NRedDecodeGuardRevision", 1);
        return KERN_SUCCESS;
    }

    struct SavedRow
    {
        UInt8 bytes[UserVideoDecodeData::MaxPatchSize];
    };
    struct SavedBuffer
    {
        SavedRow* rows{nullptr};
        SavedBuffer() { rows = IONew(SavedRow, PatchCount); }
        ~SavedBuffer()
        {
            if (rows) { IODelete(rows, SavedRow, PatchCount); }
        }
    };

    bool rollback(vm_map_t map, Page* pages, const UInt32 pageCount, const UInt64 base,
                  const UserVideoDecodeData::Patch* patches, const SavedRow* saved, const UInt32 touched)
    {
        using namespace UserVideoDecodeData;
        bool ok = true;
        for (UInt32 i = 0; i < touched; ++i) {
            const auto address = base + patches[i].offset;
            const bool ready = UserPatchMemory::prepareRollback(map, pages, pageCount, address, patches[i].size, error);
            if (!ready) {
                ok = false;
                continue;
            }
            if (writeUser(saved[i].bytes, address, patches[i].size) != 0) { ok = false; }
        }
        if (!restorePages(map, pages, pageCount)) { ok = false; }
        if (!ok) {
            stage(Stage::Rollback);
            error(KERN_FAILURE);
        }
        return ok;
    }

    struct DynamicBuffer
    {
        UserVideoDecodeResolver::Plan* plan{nullptr};
        explicit DynamicBuffer(bool enabled)
        {
            if (enabled) { plan = IONew(UserVideoDecodeResolver::Plan, 1); }
        }
        ~DynamicBuffer()
        {
            if (plan) { IODelete(plan, UserVideoDecodeResolver::Plan, 1); }
        }
    };

    bool selectFairPlayDecode(const UInt64 base, const UserVideoDecodeData::Patch* patches, UInt32& count,
                              UserVideoDecodeData::Patch* selected, const UInt32 mask)
    {
        using namespace UserVideoDecodeData;
        if (count == 0 || count > PatchCount || count > 32 || mask == 0 || (count < 32 && (mask >> count) != 0)) {
            NRed::singleton().setProp32("NRedVCNUserFpReason", 1);
            NRed::singleton().setProp32("NRedVCNUserFpCount", count);
            NRed::singleton().setProp32("NRedVCNUserFpMask", mask);
            return false;
        }
        UInt32 selectedCount{0};
        UInt8  bytes[MaxPatchSize];
        for (UInt32 i = 0; i < count; ++i) {
            const auto& patch = patches[i];
            if (patch.size == 0 || patch.size > MaxPatchSize || patch.offset >= UserSharedCache::UserLimit - base
                || patch.size >= UserSharedCache::UserLimit - base - patch.offset)
            {
                return false;
            }
            if ((mask & (1U << i)) != 0) { selected[selectedCount++] = patch; }
            else if (readUser(base + patch.offset, bytes, patch.size) != 0
                     || memcmp(bytes, patch.original, patch.size) != 0)
            {
                NRed::singleton().setProp32("NRedVCNUserFpReason", 3);
                NRed::singleton().setProp32("NRedVCNUserFpIndex", i);
                return false;
            }
        }
        count = selectedCount;
        return selectedCount != 0;
    }

    int wrappedSharedRegionCheck(proc_t process, SharedRegionCheckArgs* args, int* returnValue)
    {
        stage(Stage::Called);
        const auto result = reinterpret_cast<SharedRegionCheck>(originalCheck)(process, args, returnValue);
        if (result != 0) {
            stage(Stage::OriginalError);
            return result;
        }
        // 按 FairPlay 权限统一选择策略；客户端只允许 AMD 驱动侧解码项，
        // 保留 AppleGVA 和编码代码原样。
        bool   fairPlayDecode{false};
        UInt32 fairPlayPID{0};
        if (UserPatchPolicy::isFairPlayClient(current_task())) {
            const auto pid = process != nullptr ? proc_pid(process) : -1;
            if (!fairPlayDriverOnly) {
                if (pid > 0) { NRed::singleton().setProp32("NRedVCNFairPlaySkipPID", static_cast<UInt32>(pid)); }
                NRed::singleton().setProp32("NRedVCNFairPlaySkipped", 1);
                return result;
            }
            fairPlayDecode = true;
            if (pid > 0) { fairPlayPID = static_cast<UInt32>(pid); }
        }
        if (!VCN::allocated() || !devicePatches) {
            stage(Stage::NoMap, fairPlayPID);
            return result;
        }
        if (args == nullptr || args->startAddress == 0) {
            stage(Stage::Argument, fairPlayPID);
            return result;
        }
        using namespace UserVideoDecodeData;
        const auto* patches    = devicePatches;
        UInt32      patchCount = legacyProfile ? legacyProfile->patchCount : static_cast<UInt32>(PatchCount);
        UserVideoDecodeData::Patch legacyPatches[PatchCount];
        if (legacyProfile) {
            if (patchCount > PatchCount) { return result; }
            for (UInt32 i = 0; i < patchCount; ++i) {
                const auto& patch = legacyProfile->patches[i];
                legacyPatches[i]  = {patch.offset, patch.original, patch.patched, patch.size};
            }
            patches = legacyPatches;
        }
        UInt64 base{0};
        UInt8  header[104];
        if (readUser(args->startAddress, &base, sizeof(base)) != 0 || base == 0
            || base >= UserSharedCache::UserLimit - sizeof(header) || (base & (PAGE_SIZE - 1)) != 0
            || readUser(base, header, sizeof(header)) != 0)
        {
            stage(Stage::CacheRead, fairPlayPID);
            return result;
        }
        if (memcmp(header, CacheMagic, sizeof(CacheMagic)) != 0) {
            stage(Stage::CacheMismatch, fairPlayPID);
            return result;
        }
        // 解码规格守卫独立于渲染修复与 FairPlay 权限：只要缓存与镜像对得上就在本进程装一次。
        if (decodeGuard && !(legacyProfile && legacyNoGuard)) {
            if (auto map = getTaskMap(current_task())) {
                bool       fresh{false};
                const auto installed = installDecodeGuard(map, base, fresh);
                if (installed == KERN_SUCCESS) {
                    if (fresh) {
                        const auto count = __atomic_fetch_add(&decodeGuardApplied, 1U, __ATOMIC_RELAXED);
                        if (count < 16) { NRed::singleton().setProp32("NRedDecodeGuardApplied", count + 1); }
                        SYSLOG("UserVideoDecode", "decode guard installed in pid %d",
                               process != nullptr ? proc_pid(process) : -1);
                    }
                } else {
                    const auto count = __atomic_fetch_add(&decodeGuardRejected, 1U, __ATOMIC_RELAXED);
                    if (count < 8) {
                        NRed::singleton().setProp32("NRedDecodeGuardRejected", static_cast<UInt32>(installed));
                        SYSLOG("UserVideoDecode", "decode guard rejected: error=0x%X", installed);
                    }
                }
            }
        }

        if (fairPlayDecode) {
            NRed::singleton().setProp32("NRedDRMVideoPID", fairPlayPID);
            NRed::singleton().setProp32("NRedDRMVideoState", 1);
        }

        const char* const uuidKeys[] = {"NRedVCNUserCacheUUID0", "NRedVCNUserCacheUUID1", "NRedVCNUserCacheUUID2",
                                        "NRedVCNUserCacheUUID3"};
        for (UInt32 i = 0; i < 4; ++i) {
            UInt32 value;
            memcpy(&value, header + 88 + i * 4, sizeof(value));
            NRed::singleton().setProp32(uuidKeys[i], value);
        }
        if (legacyProfile && !UserLegacyCompat::matches(readUser, base, header + 88, *legacyProfile)) {
            stage(Stage::CacheMismatch, fairPlayPID);
            NRed::singleton().setProp32("NRedVCNUserLegacyRejected", 1);
            return result;
        }
        const bool    dynamic = !legacyProfile && memcmp(header + 88, CacheUUID, sizeof(CacheUUID)) != 0;
        DynamicBuffer dynamicBuffer(dynamic);
        if (dynamic) {
            if (!dynamicBuffer.plan) {
                error(KERN_RESOURCE_SHORTAGE);
                return result;
            }
            if (!UserVideoDecodeResolver::resolve(readUser, base, devicePatches, *dynamicBuffer.plan)) {
                stage(Stage::CacheMismatch, fairPlayPID);
                NRed::singleton().setProp32("NRedVCNUserDynStep", UserCacheResolver::diagnosticStage());
                NRed::singleton().setProp32("NRedVCNUserDynFn", UserCacheResolver::diagnosticFunction());
                NRed::singleton().setProp32("NRedVCNUserDynRef", UserCacheResolver::diagnosticReference());
                NRed::singleton().setProp32("NRedVCNUserDynImg", UserCacheResolver::diagnosticImage());
                NRed::singleton().setProp32("NRedVCNUserDynRelaxed", UserCacheResolver::diagnosticRelaxed());
                NRed::singleton().setProp32("NRedVCNUserDynamicRejected", 1);
                return result;
            }
            patches = dynamicBuffer.plan->patches;
            NRed::singleton().setProp32("NRedVCNUserDynamicMatched", 1);
            NRed::singleton().setProp32("NRedVCNUserDynamicReads", dynamicBuffer.plan->reads);
        }

        if (legacyProfile && legacyNoVCNUser) {
            NRed::singleton().setProp32("NRedVCNUserLegacySkipped", 1);
            return result;
        }

        if (fairPlayDecode && !(legacyProfile && legacyNoFairPlay)) {
            const auto mask = legacyProfile ? legacyProfile->fairPlayMask : FairPlayDecodeMask;
            if (!selectFairPlayDecode(base, patches, patchCount, legacyPatches, mask)) {
                stage(Stage::CodeMismatch, fairPlayPID);
                return result;
            }
            patches = legacyPatches;
            NRed::singleton().setProp32("NRedDRMVideoPatchCount", patchCount);
        }

        SavedBuffer buffer;
        if (!buffer.rows) {
            error(KERN_RESOURCE_SHORTAGE);
            return result;
        }
        auto*  saved = buffer.rows;
        Page   pages[MaxPageCount];
        UInt32 pageCount{0}, originals{0}, patched{0};
        for (UInt32 i = 0; i < patchCount; ++i) {
            const auto address = base + patches[i].offset;
            if (patches[i].size == 0 || patches[i].size > MaxPatchSize
                || patches[i].offset >= UserSharedCache::UserLimit - base
                || patches[i].size >= UserSharedCache::UserLimit - base - patches[i].offset
                || readUser(address, saved[i].bytes, patches[i].size) != 0)
            {
                stage(Stage::CodeRead, fairPlayPID);
                return result;
            }
            if (memcmp(saved[i].bytes, patches[i].original, patches[i].size) == 0) { ++originals; }
            else if (memcmp(saved[i].bytes, patches[i].patched, patches[i].size) == 0) {
                ++patched;
            }
            else {
                NRed::singleton().setProp32("NRedVCNUserFpReason", 4);
                NRed::singleton().setProp32("NRedVCNUserFpIndex", i);
                stage(Stage::CodeMismatch, fairPlayPID);
                error(KERN_INVALID_ARGUMENT);
                return result;
            }
            if (!UserPatchMemory::collectPages(pages, pageCount, address, patches[i].size)) {
                stage(Stage::CodeMismatch, fairPlayPID);
                return result;
            }
        }
        if (patched == patchCount) {
            stage(Stage::Already, fairPlayPID);
            if (fairPlayDecode) { NRed::singleton().setProp32("NRedDRMVideoState", 3); }
            return result;
        }
        if (originals != patchCount) {
            stage(Stage::Mixed, fairPlayPID);
            return result;
        }
        vm_map_t map = getTaskMap(current_task());
        if (map == nullptr) {
            stage(Stage::NoMap, fairPlayPID);
            return result;
        }
        for (UInt32 p = 0; p < pageCount; ++p) {
            if (!leafProtection(map, pages[p].address, pages[p].protection)
                || pages[p].protection != (VM_PROT_READ | VM_PROT_EXECUTE))
            {
                stage(Stage::Region, fairPlayPID);
                return result;
            }
        }
        // 所有匹配和保护检查完成后才进入写入；各页均先 COW、去掉 EXECUTE。
        for (UInt32 p = 0; p < pageCount; ++p) {
            const auto protection =
                vm_protect(map, pages[p].address, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
            if (protection != KERN_SUCCESS) {
                stage(Stage::Protection, fairPlayPID);
                error(protection);
                if (!restorePages(map, pages, pageCount)) { stage(Stage::Rollback, fairPlayPID); }
                return result;
            }
            pages[p].writable = true;
        }
        for (UInt32 i = 0; i < patchCount; ++i) {
            if (writeUser(patches[i].patched, base + patches[i].offset, patches[i].size) != 0) {
                stage(Stage::Write, fairPlayPID);
                error(KERN_FAILURE);
                rollback(map, pages, pageCount, base, patches, saved, i + 1);
                return result;
            }
        }
        if (!restorePages(map, pages, pageCount)) {
            stage(Stage::Restore, fairPlayPID);
            rollback(map, pages, pageCount, base, patches, saved, patchCount);
            return result;
        }
        stage(Stage::Applied, fairPlayPID);
        if (fairPlayDecode) { NRed::singleton().setProp32("NRedDRMVideoState", 2); }
        if (dynamic) { NRed::singleton().setProp32("NRedVCNUserDynamicApplied", 1); }
        if (process != nullptr) {
            const auto pid = proc_pid(process);
            char       name[32]{};
            if (pid > 0) { proc_name(pid, name, sizeof(name)); }
            name[sizeof(name) - 1] = '\0';
            if (strncmp(name, "VTDecoderXPC", 12) == 0) {
                NRed::singleton().setProp32("NRedVCNDecoderPID", static_cast<UInt32>(pid));
                SYSLOG("UserVideoDecode", "Decoder patched and RX restored: pid=%d", pid);
            }
        }
        auto count = __atomic_fetch_add(&appliedCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) { NRed::singleton().setProp32("NRedVCNUserApplied", count + 1); }
        return result;
    }

}    // namespace

void UserVideoDecode::init(KernelPatcher& patcher)
{
    fairPlayDriverOnly = false;
    if (!checkKernelArgument("-NRedVideoFix")) { return; }
    // 旧系统（14/15）用 legacyProfile 里的静态补丁数据，与 26 的动态解析路径互为独立。
    // 开关命名：legacy 作用域的开关一律把 Legacy 放到最前（-NRedLegacy*），
    // 与 26 侧开关（-NRedVideoFix / -NRedNoDecodeGuard / ...）互不为前缀。
    // 原因：Lilu checkKernelArgument 走 PE_parse_boot_arg，是前缀匹配，后缀式命名会让
    // -NRedNoDecodeGuardLegacy 顺带命中 -NRedNoDecodeGuard，把 26 上的守卫一起关掉。
    legacyNoVCNUser  = checkKernelArgument("-NRedLegacyNoVCNUser");
    legacyNoFairPlay = checkKernelArgument("-NRedLegacyNoDRMVideo");
    // NRedLegacyNoDecodeGuard = true：低版本默认不装解码守卫
    // （14 上守卫会让 VCN 解码 fence 永不 retire）。不做取反、不留反向开关。
    legacyNoGuard = true;
    decodeGuard = !checkKernelArgument("-NRedNoDecodeGuard");
    // 低版本（14.x = 23、15.x = 24）上 legacyNoGuard 为 true，等价于 NoDecodeGuard 打开，
    // 这样下面的门控与后续使用逻辑完全不用改。
    if (legacyNoGuard && currentKernelVersion().major() <= MACOS_15_X.major()) { decodeGuard = false; }
    const auto  deviceID   = NRed::singleton().getDeviceID();
    const auto* capability = VCNCapabilities::findDevice(deviceID);
    const auto  rejected   = VCNCapabilities::rejection(deviceID, currentKernelVersion());
    NRed::singleton().setProp32("NRedVCNRequested", 1);
    NRed::singleton().setProp32("NRedVCNCapabilityVersion", 2);
    NRed::singleton().setProp32("NRedVCNDeviceID", deviceID);
    if (capability) { NRed::singleton().setProp32("NRedVCNGeneration", static_cast<UInt32>(capability->generation)); }
    if (rejected) {
        NRed::singleton().setProp32("NRedVCNRejected", rejected);
        NRed::singleton().setProp32("NRedVCNUserRejected", rejected);
        SYSLOG("UserVideoDecode", "VCN capability rejected: device=0x%X reason=%u", deviceID, rejected);
        return;
    }
    devicePatches = UserVideoDecodeData::findDevice(deviceID);
    legacyProfile = UserLegacyData::video(currentKernelVersion().major(), deviceID);
    if (!devicePatches) {
        NRed::singleton().setProp32("NRedVCNUserRejected", 3);
        return;
    }
    fairPlayDriverOnly = true;
    getTaskMap         = reinterpret_cast<GetTaskMap>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_task_map"));
    patcher.clearError();
    queryRegion =
        reinterpret_cast<RegionRecurse>(patcher.solveSymbol(KernelPatcher::KernelID, "_mach_vm_region_recurse"));
    patcher.clearError();
    readUser = reinterpret_cast<CopyIn>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyin"));
    patcher.clearError();
    writeUser = reinterpret_cast<CopyOut>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyout"));
    patcher.clearError();
    if (!getTaskMap || !queryRegion || !readUser || !writeUser) {
        NRed::singleton().setProp32("NRedVCNUserRejected", 1);
        return;
    }
    KernelPatcher::RouteRequest request{"_shared_region_check_np", wrappedSharedRegionCheck, originalCheck};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCNUserRejected", 2);
        return;
    }
    NRed::singleton().setProp32("NRedVCNUserEnabled", 1);
    NRed::singleton().setProp32("NRedVCNRegionQueryVersion", 1);
    if (fairPlayDriverOnly) {
        NRed::singleton().setProp32("NRedDRMVideoEnabled", 1);
        NRed::singleton().setProp32("NRedDRMVideoRevision", 4);
        NRed::singleton().setProp32("NRedDRMVideoMask", legacyProfile ? legacyProfile->fairPlayMask :
                                                                        UserVideoDecodeData::FairPlayDecodeMask);
    }
}
