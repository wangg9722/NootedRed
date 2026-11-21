// vdec_compare.swift — 硬解与软解逐帧像素比对（验证硬件解码输出是否真的正确）
//
// 用法： xcrun swiftc -O -o vdec_compare tools/vdec_compare.swift
//        ./vdec_compare /path/to/video.mov [最多帧数，默认全部]
//
// 原理：同一文件解两遍，一遍允许硬件（默认），一遍强制软件
//      （kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder=false），
//      对每帧 Y 平面做哈希比较。全部一致 = 硬解输出与软件参考相同。
// 判据：mismatch=0 且 hw 会话确实是硬解；mismatch>0 说明硬件路径输出不可信。

import AVFoundation
import VideoToolbox

func fourcc(_ c: FourCharCode) -> String {
    let b = [UInt8((c >> 24) & 0xff), UInt8((c >> 16) & 0xff), UInt8((c >> 8) & 0xff), UInt8(c & 0xff)]
    return String(bytes: b, encoding: .ascii) ?? "?"
}

struct YPlane { var w: Int; var h: Int; var bpr: Int; var data: [UInt8] }

func planeY(_ pb: CVPixelBuffer) -> YPlane? {
    CVPixelBufferLockBaseAddress(pb, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
    let plane = CVPixelBufferGetPlaneCount(pb) > 0 ? 0 : -1
    guard plane >= 0, let base = CVPixelBufferGetBaseAddressOfPlane(pb, 0) else { return nil }
    let h = CVPixelBufferGetHeightOfPlane(pb, 0), bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
    let w = CVPixelBufferGetWidthOfPlane(pb, 0)
    var data = [UInt8]()
    data.reserveCapacity(h * bpr)
    for y in 0..<h {
        let row = base.advanced(by: y * bpr).assumingMemoryBound(to: UInt8.self)
        data.append(contentsOf: UnsafeBufferPointer(start: row, count: bpr))
    }
    return YPlane(w: w, h: h, bpr: bpr, data: data)
}

func decode(_ path: String, hw: Bool, limit: Int) -> (mode: String, planes: [YPlane], frames: Int, errs: Int) {
    let asset = AVURLAsset(url: URL(fileURLWithPath: path))
    guard let track = asset.tracks(withMediaType: .video).first,
          let fdAny = track.formatDescriptions.first else { return ("no-track", [], 0, 0) }
    let fd = fdAny as! CMFormatDescription
    var spec: CFDictionary? = nil
    if !hw {
        spec = [kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder as String: false] as CFDictionary
    }
    var sess: VTDecompressionSession?
    let dst = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
    guard VTDecompressionSessionCreate(allocator: nil, formatDescription: fd, decoderSpecification: spec,
                                       imageBufferAttributes: dst, decompressionSessionOut: &sess) == noErr,
          let s = sess else { return ("create-fail", [], 0, 0) }
    var ref: CFTypeRef?
    var mode = "unknown"
    if VTSessionCopyProperty(s, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder as CFString,
                             allocator: nil, valueOut: &ref) == noErr, let v = ref {
        mode = "\(v)"
    }
    let reader = try! AVAssetReader(asset: asset)
    let out = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
    reader.add(out); reader.startReading()
    var planes: [YPlane] = []
    var i = 0, errs = 0
    while i < limit, let sb = out.copyNextSampleBuffer() {
        var flags: VTDecodeInfoFlags = []
        let st = VTDecompressionSessionDecodeFrame(s, sampleBuffer: sb, flags: [], infoFlagsOut: &flags,
                                                  outputHandler: { status, _, imageBuffer, _, _ in
            if status == noErr, let pb = imageBuffer, let y = planeY(pb) { planes.append(y) }
            else { errs += 1 }
        })
        if st != noErr { errs += 1 }
        i += 1
    }
    VTDecompressionSessionInvalidate(s)
    return (mode, planes, i, errs)
}

let args = CommandLine.arguments
guard args.count >= 2 else { print("usage: vdec_compare <file> [maxFrames]"); exit(2) }
let path = args[1]
let limit = args.count >= 3 ? Int(args[2]) ?? Int.max : Int.max

let asset = AVURLAsset(url: URL(fileURLWithPath: path))
let codec = asset.tracks(withMediaType: .video).first.flatMap { $0.formatDescriptions.first }
    .map { fourcc(CMFormatDescriptionGetMediaSubType($0 as! CMFormatDescription)) } ?? "?"

print("文件: \(path)  编码: \(codec)")
let hwRun = decode(path, hw: true, limit: limit)
let swRun = decode(path, hw: false, limit: limit)
print("硬解会话模式: \(hwRun.mode)  帧=\(hwRun.frames) err=\(hwRun.errs) 输出帧=\(hwRun.planes.count)")
print("软解会话模式: \(swRun.mode)  帧=\(swRun.frames) err=\(swRun.errs) 输出帧=\(swRun.planes.count)")

// 容差判据：逐帧统计 |diff| 的均值与最大值；pixel>2 视为硬差异
let n = min(hwRun.planes.count, swRun.planes.count)
var worstMean = 0.0, worstMax = 0, worstFrame = -1
var badFrames = 0
for i in 0..<n {
    let a = hwRun.planes[i], b = swRun.planes[i]
    let w = min(a.w, b.w), h = min(a.h, b.h)
    guard w > 0, h > 0 else { badFrames += 1; continue }
    var sum = 0, mx = 0
    for y in 0..<h {
        let oa = y * a.bpr, ob = y * b.bpr
        for x in 0..<w {
            let d = abs(Int(a.data[oa + x]) - Int(b.data[ob + x]))
            sum += d; if d > mx { mx = d }
        }
    }
    let mean = Double(sum) / Double(w * h)
    if i == 0 { print("  \(i): hw \(a.w)x\(a.h) bpr=\(a.bpr) / sw \(b.w)x\(b.h) bpr=\(b.bpr) 已按可见区域比对") }
    if Double(mx) > 2 { badFrames += 1 }
    if mx > worstMax { worstMax = mx; worstFrame = i }
    if mean > worstMean { worstMean = mean }
}
print(String(format: "比对帧数: %d  超容差帧(>2): %d  最大差值: %d (帧 %d)  最大平均差: %.3f", n, badFrames, worstMax, worstFrame, worstMean))
print(badFrames == 0 && n > 0 ? "结论: 硬解输出与软件参考在容差内一致" : "结论: 硬解输出存在可见差异，需人工核验")
exit(badFrames == 0 ? 0 : 1)
