// 极简工具：列出 CoreAudio 输出设备 / 切换系统默认输出 / 查询当前默认输出
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int devName(AudioDeviceID dev, char *out, size_t n)
{
    CFStringRef name = NULL;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &name) != noErr || !name) { return -1; }
    CFStringGetCString(name, out, (CFIndex)n, kCFStringEncodingUTF8);
    CFRelease(name);
    return 0;
}

static int hasOutput(AudioDeviceID dev)
{
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr) { return 0; }
    return size > 0;
}

int main(int argc, char **argv)
{
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr) { return 1; }
    UInt32 count = size / sizeof(AudioDeviceID);
    AudioDeviceID *devs = calloc(count, sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devs);

    AudioDeviceID def = 0;
    AudioObjectPropertyAddress da = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
    UInt32 dsz = sizeof(def);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &dsz, &def);

    if (argc == 1 || strcmp(argv[1], "list") == 0) {
        for (UInt32 i = 0; i < count; ++i) {
            char n[256] = {0};
            devName(devs[i], n, sizeof(n));
            if (!hasOutput(devs[i])) { continue; }
            printf("%u\t%s%s\n", devs[i], n, devs[i] == def ? "\t[DEFAULT]" : "");
        }
        return 0;
    }

    if (strcmp(argv[1], "get") == 0) {
        char n[256] = {0};
        devName(def, n, sizeof(n));
        printf("%u\t%s\n", def, n);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0 && argc >= 3) {
        AudioDeviceID target = 0;
        if (sscanf(argv[2], "%u", &target) != 1 && strlen(argv[2]) == 0) { return 2; }
        if (target == 0) {
            // 按名字子串匹配
            for (UInt32 i = 0; i < count; ++i) {
                char n[256] = {0};
                devName(devs[i], n, sizeof(n));
                if (hasOutput(devs[i]) && strstr(n, argv[2])) { target = devs[i]; break; }
            }
        }
        if (target == 0) { fprintf(stderr, "no device\n"); return 3; }
        AudioObjectPropertyAddress sa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
        if (AudioObjectSetPropertyData(kAudioObjectSystemObject, &sa, 0, NULL, sizeof(target), &target) != noErr) {
            fprintf(stderr, "set failed\n");
            return 4;
        }
        char n[256] = {0};
        devName(target, n, sizeof(n));
        printf("default -> %u\t%s\n", target, n);
        return 0;
    }
    fprintf(stderr, "usage: audioout [list|get|set <id|name>]\n");
    return 2;
}
