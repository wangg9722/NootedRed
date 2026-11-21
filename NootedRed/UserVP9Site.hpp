// VP9 站点表的公共描述类型：各系统版本的数据表共用同一结构，运行时按内核版本选表。
#pragma once
#include <IOKit/IOTypes.h>

namespace UserVP9
{
    struct Site
    {
        UInt64       offset;      // 相对主缓存 unslid 基址
        UInt32       size;        // 原始字节校验长度
        UInt32       patchSize;   // 实际写入长度（注入型站点可以长于校验区）
        const UInt8* original;
        const UInt8* patched;
    };

    struct Table
    {
        const UInt8* uuid;       // 主缓存 UUID（16 字节）
        UInt32       siteCount;
        const Site*  sites;
    };
}    // namespace UserVP9
