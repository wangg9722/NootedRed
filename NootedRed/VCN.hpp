// Vega 核显的 VCN2 接入；保持默认关闭，具体状态由真实初始化路径发布。
#pragma once
#include <Headers/kern_patcher.hpp>

namespace VCN
{

    void  init();
    void  processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);
    bool  ready();
    bool  allocated();
    void* allocateEngine();
    // 仅 debug 分支：记录硬解初始化链路上各节点的发生顺序与时刻（code<<8|detail，ms 另存）。
    void  traceInit(UInt32 code, UInt32 detail);

}    // namespace VCN
