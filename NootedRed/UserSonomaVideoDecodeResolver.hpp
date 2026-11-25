// Darwin 23（macOS 14）视频解码补丁计划。
// 数据来自 steve12311/NootedRed 的 UserSonomaVideoData.hpp（CacheUUID 与 14.8.9 缓存一致），
// 按「image + 整函数指纹 + 注入点」定位，不依赖任何写死的成员偏移。
#pragma once
#include <UserSonomaVideoData.hpp>

namespace UserSonomaVideoDecodeResolver
{

    using namespace UserSonomaVideoData;
    inline constexpr UInt32 SiteCount = sizeof(Patches15E7) / sizeof(Patches15E7[0]);

    struct Plan
    {
        UserSonomaVideoData::Patch patches[SiteCount]{};
        UInt8                      original[SiteCount][MaxPatchSize]{};
        UInt8                      patched[SiteCount][MaxPatchSize]{};
        UInt32                     reads{0};
    };

    inline bool resolve(UserSharedCache::Reader reader, UInt64 base, const UserSonomaVideoData::Patch* patches,
                        Plan& plan)
    {
        UserSharedCache::Input input{reader, {}};
        if (!patches
            || !UserCacheResolver::resolve(input, base, ResolverImages, ResolverFunctions, patches, ResolverLocations,
                                           plan.patches, &plan.original[0][0], &plan.patched[0][0], MaxPatchSize))
        {
            return false;
        }
        plan.reads = input.reads;
        return true;
    }

}    // namespace UserSonomaVideoDecodeResolver
