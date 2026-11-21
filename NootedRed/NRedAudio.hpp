// HDMI / DisplayPort Audio Topology & Engine
//
// Copyright © 2022-2026 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// 用于修复「外接显示器音频设备可见但无声」（上游 issue #225）。
// 思路：AppleGFXHDA 只负责 HDA 控制器侧并喂 DMA，DCN 显示侧的音频管线
// （DCCG Audio DTO / Azalia endpoint / DIG AFMT / DP_SEC）在 APU 上默认没有被正确配置，
// 本模块在「framebuffer 上电」「DP 链路重新上电」「音频控制器 probe」这三个既有事件点上
// 把这条管线配起来；不引入任何定时器、不按固定时长等待，配置失败就等下一个自然事件。

#pragma once
#include <IOKit/IOTypes.h>

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

    // 由 NRed 初始化时调用。默认启用，无需任何 boot-arg：
    //   -NRedNoAudio：整体关闭本模块；
    //   -NRedAudioProbe：只读诊断模式，发布寄存器快照但绝不写寄存器。
    bool enabled() const;
    bool probeOnly() const;

    // 事件驱动入口，全部由既有驱动调用点触发，不启动轮询线程、不做延时重试。
    void onControllerPowerUp();
    void onDisplayLinkSettled();

    // 只读的拓扑快照，写入 IORegistry（NRedAudio* 属性）。
    void probeTopology();

    // 事件驱动的 DCN 音频配置（DCCG DTO + Azalia endpoint + AFMT + DP_SEC）。
    // 外接 framebuffer 尚未激活、或 DIG 后端尚未使能时返回 false（此时只记一个待办属性，
    // 由下一个自然事件再试，不做定时器轮询）。
    bool configureAudio(bool pulseDpSec = false);

private:
    bool controllerPoweredUp{false};
    bool externalLinkActive();

    UInt32 passes{0};

    static auto resolveDcnIpBlock() -> DcnIpBlock;
};
