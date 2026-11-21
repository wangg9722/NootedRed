// 硬件 VP9 解码站点安装（一份 kext 支持多个系统版本，按内核版本选表）。
//
// 站点数据（偏移 + 原始字节 + 补丁字节）由 tools/gen_vp9_sites.py 从参考实现的公开数据集生成，
// 生成时已经用对应机器的真实 dyld 缓存逐站点做过唯一性校验；运行时再校验主缓存 UUID 与原始字节。
// 数据来源与致谢见 docs/vp9-hw-decode-analysis.md。

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatchMemory.hpp>
#include <UserVP9.hpp>
#include <UserVP9Site.hpp>
#include <UserVP9SitesDarwin23.hpp>
#include <UserVP9SitesDarwin25.hpp>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

namespace
{
    using UserPatchMemory::Page;

    // 与 UserSurfaceSyncResolver 相同的用户地址上限，用于挡住越界地址。
    constexpr UInt64 UserLimit    = 0x00007FFFFFFFFFFFULL;
    constexpr UInt32 MaxSites     = 64;
    constexpr UInt32 PageCapacity = 24;

    enum class VP9State : UInt32
    {
        Off       = 0,
        Resolving = 1,
        Ready     = 2,
        Failed    = 3,
    };

    struct Context
    {
        IOLock*               lock{nullptr};
        UInt8*                saved{nullptr};
        UInt32                spanStart[MaxSites]{};
        Page                  pages[PageCapacity]{};
        UInt32                pageCount{0};
        UInt32                applied{0};
        const UserVP9::Table* table{nullptr};
        bool                  published{false};
        bool                  withdrawn{false};
    };

    Context  context;
    VP9State state{VP9State::Off};
    bool     enabled{false};

    inline UInt32 spanOf(const UserVP9::Site& site)
    {
        return site.size > site.patchSize ? site.size : site.patchSize;
    }

    void setState(VP9State value)
    {
        state = value;
        NRed::singleton().setProp32("NRedVP9State", static_cast<UInt32>(value));
    }

    // 同一份 kext 内按内核版本选表：每张表都用对应版本自己的 dyld 缓存生成，UUID 不符即拒绝。
    const UserVP9::Table* selectTable()
    {
        switch (getKernelVersion()) {
            case KernelVersion::Tahoe:
                return &UserVP9SitesDarwin25::table;
            case KernelVersion::Sonoma:
                return &UserVP9SitesDarwin23::table;
            default:
                return nullptr;
        }
    }

    IOService* findVideoAccelerator()
    {
        // 只认真正挂着 AMDRadeonVADriver2 的加速器服务，避免把能力键发到别的显卡上。
        auto* matching = IOService::serviceMatching("IOAccelerator");
        if (matching == nullptr) { return nullptr; }
        IOService*  found    = nullptr;
        OSIterator* iterator = IOService::getMatchingServices(matching);
        if (iterator != nullptr) {
            OSObject* entry = nullptr;
            while (!found && (entry = iterator->getNextObject()) != nullptr) {
                auto* service = OSDynamicCast(IOService, entry);
                if (service == nullptr) { continue; }
                auto* bundle = OSDynamicCast(OSString, service->getProperty("IODVDBundleName"));
                if (bundle == nullptr) { continue; }
                if (strcmp(bundle->getCStringNoCopy(), "AMDRadeonVADriver2") == 0) { found = service; }
            }
            iterator->release();
        }
        return found;
    }

    void setStringProperty(IOService* service, const char* key, const char* value)
    {
        auto* object = OSString::withCString(value);
        if (object == nullptr) { return; }
        service->setProperty(key, object);
        object->release();
    }

    void publishCapabilities()
    {
        if (context.published || context.withdrawn) { return; }
        auto* service = findVideoAccelerator();
        if (service == nullptr) { return; }
        auto* profiles   = OSArray::withCapacity(2);
        auto* dictionary = OSDictionary::withCapacity(1);
        if (profiles == nullptr || dictionary == nullptr) {
            if (profiles) { profiles->release(); }
            if (dictionary) { dictionary->release(); }
            return;
        }
        profiles->setObject(OSNumber::withNumber(static_cast<UInt32>(0), 32));
        profiles->setObject(OSNumber::withNumber(static_cast<UInt32>(2), 32));
        dictionary->setObject("VTSupportedProfileArray", profiles);
        setStringProperty(service, "IOGVAXDecode", "2");
        setStringProperty(service, "IOGVAVP9Decode", "1");
        service->setProperty("IOGVAVP9DecodeCapabilities", dictionary);
        profiles->release();
        dictionary->release();
        context.published = true;
        NRed::singleton().setProp32("NRedVP9Published", 1);
        SYSLOG("UserVP9", "VP9 capabilities published on accelerator service");
    }

    void withdrawCapabilities()
    {
        if (context.withdrawn) { return; }
        context.withdrawn = true;
        context.published = false;
        auto* service = findVideoAccelerator();
        if (service == nullptr) { return; }
        service->removeProperty("IOGVAVP9DecodeCapabilities");
        service->removeProperty("IOGVAVP9Decode");
        service->removeProperty("IOGVAXDecode");
        NRed::singleton().setProp32("NRedVP9Withdrawn", 1);
        SYSLOG("UserVP9", "VP9 capabilities withdrawn after patch failure");
    }

    void reportError(const kern_return_t value)
    {
        NRed::singleton().setProp32("NRedVP9LastVMError", static_cast<UInt32>(value == 0 ? 1 : value));
    }
}    // namespace

bool UserVP9::isEnabled() { return enabled; }

void UserVP9::init()
{
    // 首飞阶段必须显式 -NRedVP9 才启用；-NRedNoVP9 优先级最高。
    if (checkKernelArgument("-NRedNoVP9") || !checkKernelArgument("-NRedVP9")) {
        setState(VP9State::Off);
        return;
    }
    context.table = selectTable();
    if (context.table == nullptr || context.table->siteCount == 0 || context.table->siteCount > MaxSites) {
        setState(VP9State::Off);
        SYSLOG("UserVP9", "No site table for Darwin %u", currentKernelVersion().major());
        return;
    }
    context.lock = IOLockAlloc();
    if (context.lock == nullptr) {
        setState(VP9State::Failed);
        return;
    }
    UInt32 total = 0;
    for (UInt32 i = 0; i < context.table->siteCount; ++i) {
        context.spanStart[i] = total;
        total += spanOf(context.table->sites[i]);
    }
    context.saved = static_cast<UInt8*>(IOMalloc(total));
    if (context.saved == nullptr) {
        setState(VP9State::Failed);
        return;
    }
    enabled = true;
    setState(VP9State::Resolving);
    NRed::singleton().setProp32("NRedVP9SiteCount", context.table->siteCount);
    SYSLOG("UserVP9", "VP9 patch table armed: %u sites, %u bytes saved, Darwin %u", context.table->siteCount,
           total, currentKernelVersion().major());
}

void UserVP9::apply(const UInt64 base, const UInt8* uuid, const ReadUser read, const WriteUser write,
                    const vm_map_t map, const RegionQuery query)
{
    using namespace UserPatchMemory;
    if (!enabled || context.lock == nullptr || context.saved == nullptr || context.table == nullptr) { return; }
    if (state == VP9State::Failed || state == VP9State::Off) { return; }
    if (read == nullptr || write == nullptr || map == nullptr || query == nullptr) { return; }
    if (uuid == nullptr || memcmp(uuid, context.table->uuid, sizeof(UInt8) * 16) != 0) {
        NRed::singleton().setProp32("NRedVP9UUIDMismatch", 1);
        return;
    }

    IOLockLock(context.lock);

    const auto siteCount = context.table->siteCount;
    if (siteCount < 2) {
        IOLockUnlock(context.lock);
        return;
    }

    // 先探读一个站点：没有映射 AMDRadeonVADriver2 的进程会在这里直接返回，
    // 不需要逐个进程做几十 KB 的读取与写入。
    {
        const auto& probe = context.table->sites[1];
        UInt8       bytes[16];
        if (probe.size < sizeof(bytes) || read(base + probe.offset, bytes, sizeof(bytes)) != 0) {
            IOLockUnlock(context.lock);
            return;
        }
    }

    UInt32 originals = 0, patched = 0;
    for (UInt32 i = 0; i < siteCount; ++i) {
        const auto&  site    = context.table->sites[i];
        const auto   address = base + site.offset;
        auto* const  saved   = context.saved + context.spanStart[i];
        const UInt32 span    = spanOf(site);
        if (address >= UserLimit - span) { break; }
        if (read(address, saved, span) != 0) {
            NRed::singleton().setProp32("NRedVP9ReadFailed", i + 1);
            IOLockUnlock(context.lock);
            return;
        }
        if (memcmp(saved, site.original, site.size) == 0) {
            ++originals;
        } else if (memcmp(saved, site.patched, site.patchSize) == 0) {
            ++patched;
        } else {
            // 站点字节与预期不符：驱动版本或缓存布局变了，撤回能力键，保持软解路径可用。
            NRed::singleton().setProp32("NRedVP9MismatchSite", i);
            setState(VP9State::Failed);
            IOLockUnlock(context.lock);
            withdrawCapabilities();
            return;
        }
    }
    if (patched == siteCount) {
        setState(VP9State::Ready);
        publishCapabilities();
        IOLockUnlock(context.lock);
        return;
    }
    if (originals != siteCount) {
        NRed::singleton().setProp32("NRedVP9Mixed", 1);
        IOLockUnlock(context.lock);
        return;
    }

    UInt32 pageCount = 0;
    for (UInt32 i = 0; i < siteCount; ++i) {
        const auto& site = context.table->sites[i];
        if (!collectPages(context.pages, pageCount, base + site.offset, site.patchSize)
            || pageCount > PageCapacity)
        {
            NRed::singleton().setProp32("NRedVP9PageOverflow", pageCount);
            IOLockUnlock(context.lock);
            return;
        }
    }
    for (UInt32 p = 0; p < pageCount; ++p) {
        auto& page = context.pages[p];
        if (!leafProtection(map, page.address, page.protection, query, reportError)) {
            IOLockUnlock(context.lock);
            return;
        }
        if ((page.protection & VM_PROT_READ) == 0) {
            IOLockUnlock(context.lock);
            return;
        }
    }

    // 全部校验通过后才进入写入：先整批 COW 去执行权限，再逐站点写入，失败则回滚已写站点。
    for (UInt32 p = 0; p < pageCount; ++p) {
        if (!makeWritable(map, context.pages[p], reportError)) {
            restorePages(map, context.pages, pageCount, reportError);
            IOLockUnlock(context.lock);
            return;
        }
    }
    UInt32 written = 0;
    for (UInt32 i = 0; i < siteCount; ++i) {
        const auto& site    = context.table->sites[i];
        const auto  address = base + site.offset;
        if (write(site.patched, address, site.patchSize) != 0) {
            NRed::singleton().setProp32("NRedVP9WriteFailed", i + 1);
            for (UInt32 j = 0; j < written; ++j) {
                const auto& done = context.table->sites[j];
                write(context.saved + context.spanStart[j], base + done.offset, done.patchSize);
            }
            restorePages(map, context.pages, pageCount, reportError);
            setState(VP9State::Failed);
            IOLockUnlock(context.lock);
            withdrawCapabilities();
            return;
        }
        ++written;
    }
    if (!restorePages(map, context.pages, pageCount, reportError)) {
        NRed::singleton().setProp32("NRedVP9RestoreFailed", 1);
        IOLockUnlock(context.lock);
        return;
    }
    ++context.applied;
    NRed::singleton().setProp32("NRedVP9Applied", context.applied);
    setState(VP9State::Ready);
    publishCapabilities();
    IOLockUnlock(context.lock);
    SYSLOG("UserVP9", "VP9 sites installed, process count %u", context.applied);
}
