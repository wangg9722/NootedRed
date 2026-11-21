// 硬件 VP9 解码站点安装。
//
// 站点数据（偏移 + 原始字节 + 补丁字节）由 tools/gen_vp9_sites.py 从参考实现的公开数据集生成，
// 生成时已经用本机真实 dyld 缓存逐站点做过唯一性校验，并在运行时再次校验缓存 UUID 与原始字节。
// 数据来源与致谢见 docs/vp9-hw-decode-analysis.md。

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <NRed.hpp>
#include <UserPatchMemory.hpp>
#include <UserVP9.hpp>
#include <UserVP9SitesDarwin25.hpp>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

namespace
{
    using UserPatchMemory::Page;

    // 与 UserSurfaceSyncResolver 相同的用户地址上限，用于挡住越界地址。
    constexpr UInt64 UserLimit = 0x00007FFFFFFFFFFFULL;

    enum class VP9State : UInt32
    {
        Off      = 0,
        Resolving = 1,
        Ready    = 2,
        Failed   = 3,
    };

    constexpr UInt32 PageCapacity = 24;
    constexpr UInt32 SiteCount    = UserVP9SitesDarwin25::SiteCount;
    static_assert(SiteCount <= 64, "站点数量超出预期");

    struct Context
    {
        IOLock* lock{nullptr};
        UInt8*  saved{nullptr};
        UInt32  spanStart[SiteCount]{};
        Page    pages[PageCapacity]{};
        UInt32  pageCount{0};
        UInt32  applied{0};
        bool    published{false};
        bool    withdrawn{false};
    };

    Context context;
    VP9State state{VP9State::Off};
    bool     enabled{false};

    inline UInt32 spanOf(const UserVP9SitesDarwin25::Site& site)
    {
        return site.size > site.patchSize ? site.size : site.patchSize;
    }

    void setState(VP9State value)
    {
        state = value;
        NRed::singleton().setProp32("NRedVP9State", static_cast<UInt32>(value));
    }

    IOService* findVideoAccelerator()
    {
        // 只认真正挂着 AMDRadeonVADriver2 的加速器服务，避免把能力键发到别的显卡上。
        auto* matching = IOService::serviceMatching("IOAccelerator");
        if (matching == nullptr) { return nullptr; }
        IOService*  found = nullptr;
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
        auto* profiles = OSArray::withCapacity(2);
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
}

void UserVP9::init()
{
    // 首飞阶段必须显式 -NRedVP9 才启用；字节站点全部通过校验后才对外发布能力键。
    if (getKernelVersion() != KernelVersion::Tahoe || checkKernelArgument("-NRedNoVP9")
        || !checkKernelArgument("-NRedVP9"))
    {
        setState(VP9State::Off);
        return;
    }
    context.lock = IOLockAlloc();
    if (context.lock == nullptr) {
        setState(VP9State::Failed);
        return;
    }
    UInt32 total = 0;
    for (UInt32 i = 0; i < SiteCount; ++i) {
        context.spanStart[i] = total;
        total += spanOf(UserVP9SitesDarwin25::Sites[i]);
    }
    context.saved = static_cast<UInt8*>(IOMalloc(total));
    if (context.saved == nullptr) {
        setState(VP9State::Failed);
        return;
    }
    enabled = true;
    setState(VP9State::Resolving);
    SYSLOG("UserVP9", "VP9 patch table armed: %u sites, %u bytes of saved bytes", SiteCount, total);
}

bool UserVP9::isEnabled() { return enabled; }

void UserVP9::apply(const UInt64 base, const UInt8* uuid, const ReadUser read, const WriteUser write,
                    const vm_map_t map, const RegionQuery query)
{
    using namespace UserPatchMemory;
    if (!enabled || context.lock == nullptr || context.saved == nullptr) { return; }
    if (state == VP9State::Failed || state == VP9State::Off) { return; }
    if (read == nullptr || write == nullptr || map == nullptr || query == nullptr) { return; }
    if (uuid == nullptr || memcmp(uuid, UserVP9SitesDarwin25::UUID, sizeof(UserVP9SitesDarwin25::UUID)) != 0) {
        NRed::singleton().setProp32("NRedVP9UUIDMismatch", 1);
        return;
    }

    IOLockLock(context.lock);

    // 先探一下最小站点所在的可读性：没有映射 AMDRadeonVADriver2 的进程会在这里直接返回，
    // 不需要逐个进程做 26 KB 的读取与写入。
    {
        const auto& probe = UserVP9SitesDarwin25::Sites[1];
        UInt8       bytes[16];
        if (probe.size < sizeof(bytes) || read(base + probe.offset, bytes, sizeof(bytes)) != 0) {
            IOLockUnlock(context.lock);
            return;
        }
    }

    UInt32 originals = 0, patched = 0;
    for (UInt32 i = 0; i < SiteCount; ++i) {
        const auto& site     = UserVP9SitesDarwin25::Sites[i];
        const auto  address  = base + site.offset;
        auto* const saved    = context.saved + context.spanStart[i];
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
            // 站点字节和预期不符：说明驱动版本或缓存布局变了，撤回能力键，保持软解路径可用。
            NRed::singleton().setProp32("NRedVP9MismatchSite", i);
            setState(VP9State::Failed);
            IOLockUnlock(context.lock);
            withdrawCapabilities();
            return;
        }
    }
    if (patched == SiteCount) {
        setState(VP9State::Ready);
        publishCapabilities();
        IOLockUnlock(context.lock);
        return;
    }
    if (originals != SiteCount) {
        NRed::singleton().setProp32("NRedVP9Mixed", 1);
        IOLockUnlock(context.lock);
        return;
    }

    UInt32 pageCount = 0;
    for (UInt32 i = 0; i < SiteCount; ++i) {
        const auto&  site    = UserVP9SitesDarwin25::Sites[i];
        const UInt32 span    = site.patchSize;
        if (!collectPages(context.pages, pageCount, base + site.offset, span) || pageCount > PageCapacity) {
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
    for (UInt32 i = 0; i < SiteCount; ++i) {
        const auto& site    = UserVP9SitesDarwin25::Sites[i];
        const auto  address = base + site.offset;
        if (write(site.patched, address, site.patchSize) != 0) {
            NRed::singleton().setProp32("NRedVP9WriteFailed", i + 1);
            for (UInt32 j = 0; j < written; ++j) {
                const auto& done = UserVP9SitesDarwin25::Sites[j];
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
