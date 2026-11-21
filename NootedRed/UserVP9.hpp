// 硬件 VP9 解码：把参考实现验证过的字节站点写入每进程的 dyld 共享缓存副本，并在加速器服务上
// 发布 VP9 能力键。仅做字节级移植，不依赖参考实现的解析器与运行时。
#pragma once
#include <Headers/kern_patcher.hpp>
#include <mach/vm_types.h>
#include <mach/vm_region.h>

namespace UserVP9
{
    using RegionQuery = kern_return_t (*)(vm_map_t, mach_vm_address_t*, mach_vm_size_t*, natural_t*,
                                          vm_region_recurse_info_t, mach_msg_type_number_t*);
    using ReadUser    = int (*)(user_addr_t, void*, size_t);
    using WriteUser   = int (*)(const void*, user_addr_t, size_t);

    // 无需 hook 新符号：由 UserSurfaceSync 在 _shared_region_check_np 回调里转发进来。
    void init();
    bool isEnabled();
    void apply(UInt64 base, const UInt8* uuid, ReadUser read, WriteUser write, vm_map_t map, RegionQuery query);
}    // namespace UserVP9
