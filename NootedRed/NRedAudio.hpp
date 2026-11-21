// HDMI / DisplayPort Audio Topology & Engine
//
// Copyright © 2022-2026 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// 用于修复「外接显示器音频设备可见但无声」（上游 issue #225）。
// 思路：AppleGFXHDA 只负责 HDA 控制器侧并喂 DMA，DCN 显示侧的音频管线
// （DCCG Audio DTO / Azalia endpoint / DIG AFMT / DP_SEC）在 APU 上默认没有被正确配置，
// 本模块在 framebuffer 上电与链路能力上报这两个事件点把这条管线配起来。

#pragma once
#include <IOKit/IOTypes.h>
#include <kern/thread_call.h>

class NRedAudio
{
public:
    enum class DcnGen : UInt8 {
        kUnknown = 0,
        kDcn10,
        kDcn21,
    };

    struct DcnIpBlock
    {
        DcnGen gen{DcnGen::kUnknown};
        UInt32 dccgBase{0};
        UInt32 dcnBase{0};
        UInt32 otgMasterEnOffset{0};
        UInt32 digStride{0x100};
        UInt32 otgStride{0x080};
        UInt32 maxDig{5};
        UInt32 maxOtg{4};
        UInt32 maxAzEp{4};
    };

    static NRedAudio& singleton();

    // 由 NRed 初始化时调用：读取 -NRedAudioProbe / -NRedAudioApply 决定是否参与。
    //   -NRedAudioProbe：只做寄存器读取与属性发布，绝不写寄存器（先验证偏移安全）；
    //   -NRedAudioApply：完整编程（DTO + Azalia + AFMT + DP_SEC）。
    void init();
    bool enabled() const;

    // 事件驱动入口，不启动轮询线程。
    void onControllerPowerUp();
    void onLinkCapabilitiesReported();

    // 只读的拓扑快照，写入 IORegistry（NRedAudio* 属性）。
    void probeTopology();

    // 事件驱动的 DCN 音频配置（DCCG DTO + Azalia endpoint + AFMT + DP_SEC）。
    // 外接 framebuffer 已激活但其 DIG 后端尚未使能时返回 false，交由调用方重试。
    bool configureAudio(bool pulseDpSec = false);

private:
    thread_call_t delayedProbeCall{nullptr};
    UInt32        settleRetries{0};
    bool          postLinkPassDone{false};
    bool          active{false};
    bool          probeOnly{false};

    // 写入阶段位（只有对应开关打开才写某一处寄存器，避免一次全写把时钟树/链路写坏）
    static constexpr UInt32 kStageAz    = 1U << 0;    // Azalia endpoint（AppleGFXHDA 已在写，风险较高）
    static constexpr UInt32 kStageAfmt  = 1U << 1;    // DIG AFMT（音频信息帧/包使能）
    static constexpr UInt32 kStageDpSec = 1U << 2;    // DP_SEC（DP 音频安全/流使能）
    static constexpr UInt32 kStageDto   = 1U << 3;    // DCCG Audio DTO（音频时钟，最危险）
    static constexpr UInt32 kStageAll   = 0xFU;

    UInt32      stageMask{0};
    UInt32      consoleWaitTries{0};
    static constexpr UInt32 kConsoleWaitMax = 120;    // 最多等 120s 图形会话建立

    void        scheduleDelayedProbe(UInt32 delayMs = 2000);
    static void delayedProbeCallback(thread_call_param_t param0, thread_call_param_t param1);
    static auto resolveDcnIpBlock() -> DcnIpBlock;
};
