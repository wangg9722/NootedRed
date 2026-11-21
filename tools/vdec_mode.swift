// vdec_mode.swift — 一条命令判断视频走硬解还是软解（VideoToolbox 会话级属性，最权威）
//
// 用法：
//   swift vdec_mode.swift /path/to/video.mov [最多解码帧数，默认 30]
//   或编译后： clang/swiftc -O -o vdec_mode vdec_mode.swift  （xcrun swiftc）
//
// 判据：VTDecompressionSession 启动解码后查询
//   kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder
//   1 = 硬解（走 VCN/VADriver2），0 或 unknown = 软解（Apple 软件解码器）
// 注意：该属性只对「本次会话正在解的这个文件」有效；想在别的应用里判断，
//       等价做法是看该进程里 VTDecoderXPCService 的 CPU（软解明显高），但不如这个准。

import AVFoundation
import VideoToolbox

func fourcc(_ c: FourCharCode) -> String {
    let bytes = [UInt8((c >> 24) & 0xff), UInt8((c >> 16) & 0xff), UInt8((c >> 8) & 0xff), UInt8(c & 0xff)]
    return String(bytes: bytes, encoding: .ascii) ?? "?"
}

let args = CommandLine.arguments
guard args.count >= 2 else { print("usage: vdec_mode <file> [maxFrames]"); exit(2) }
let path = args[1]
let maxFrames = args.count >= 3 ? Int(args[2]) ?? 30 : 30

let asset = AVURLAsset(url: URL(fileURLWithPath: path))
guard let track = asset.tracks(withMediaType: .video).first,
      let fdAny = track.formatDescriptions.first else { print("没有视频轨"); exit(3) }
let fd = fdAny as! CMFormatDescription
let dims = CMVideoFormatDescriptionGetDimensions(fd)

var codecName = fourcc(CMFormatDescriptionGetMediaSubType(fd))
if codecName == "hvc1" || codecName == "hev1" { codecName += " (HEVC)" }
else if codecName == "avc1" || codecName == "avc3" { codecName += " (H.264)" }
else if codecName == "vp09" { codecName += " (VP9)" }
else if codecName == "av01" { codecName += " (AV1)" }

var sess: VTDecompressionSession?
let dst = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
let st = VTDecompressionSessionCreate(allocator: nil, formatDescription: fd, decoderSpecification: nil,
                                      imageBufferAttributes: dst, decompressionSessionOut: &sess)
guard st == noErr, let s = sess else { print("会话创建失败 status=\(st)"); exit(3) }

func hwFlag() -> String {
    var ref: CFTypeRef?
    guard VTSessionCopyProperty(s, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder as CFString,
                                allocator: nil, valueOut: &ref) == noErr, let v = ref else { return "unknown" }
    if let n = v as? Int { return n == 0 ? "0(软解)" : "1(硬解)" }
    return String(describing: v)
}

let reader = try! AVAssetReader(asset: asset)
let out = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
reader.add(out); reader.startReading()

var i = 0, errs = 0
var hwAfter = "unknown"
while i < maxFrames, let sample = out.copyNextSampleBuffer() {
    var flags: VTDecodeInfoFlags = []
    let ds = VTDecompressionSessionDecodeFrame(s, sampleBuffer: sample, flags: [], infoFlagsOut: &flags,
                                               outputHandler: { status, _, _, _, _ in })
    if ds != noErr { errs += 1 }
    if i == 3 { hwAfter = hwFlag() }
    i += 1
}
VTDecompressionSessionInvalidate(s)

print("文件: \(path)")
print("编码: \(codecName)  \(dims.width)x\(dims.height)")
print("解码帧数: \(i)  错误帧: \(errs)")
print("硬件解码: \(hwAfter)")
print(hwAfter.hasPrefix("1") ? "结论: 硬解（VCN 引擎）" : "结论: 软解（Apple 软件解码器，吃 CPU）")
exit(0)
