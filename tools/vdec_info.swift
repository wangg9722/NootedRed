// vdec_info.swift — 打印视频编码参数（profile/位深/chroma）并测试硬解/软解会话能否创建
import AVFoundation
import VideoToolbox

func fourcc(_ c: FourCharCode) -> String {
    let b = [UInt8((c >> 24) & 0xff), UInt8((c >> 16) & 0xff), UInt8((c >> 8) & 0xff), UInt8(c & 0xff)]
    return String(bytes: b, encoding: .ascii) ?? "?"
}

for path in CommandLine.arguments.dropFirst() {
    let asset = AVURLAsset(url: URL(fileURLWithPath: path))
    guard let track = asset.tracks(withMediaType: .video).first,
          let fdAny = track.formatDescriptions.first else { print("\(path): 没有视频轨"); continue }
    let fd = fdAny as! CMFormatDescription
    let dims = CMVideoFormatDescriptionGetDimensions(fd)
    print("文件: \((path as NSString).lastPathComponent)  \(dims.width)x\(dims.height)  \(fourcc(CMFormatDescriptionGetMediaSubType(fd)))")

    if let ext = CMFormatDescriptionGetExtensions(fd) as? [String: Any] {
        if let atoms = ext["SampleDescriptionExtensionAtoms"] as? [String: Any], let hvcC = atoms["hvcC"] as? Data {
            let b = [UInt8](hvcC)
            if b.count > 12 {
                let profileByte = b[1]
                let profileIdc = profileByte & 0x1f
                let compat = UInt32(b[2]) << 24 | UInt32(b[3]) << 16 | UInt32(b[4]) << 8 | UInt32(b[5])
                let tierFlag = (profileByte >> 5) & 1
                let chroma = Int(b[6] & 0x03)
                let bdL = Int(b[9] & 0x07) + 8
                let bdC = Int(b[10] & 0x07) + 8
                print("  hvcC: profile_idc=\(profileIdc) tier=\(tierFlag) compat=\(String(format: "%08x", compat)) chroma=\(chroma)(\(chroma == 1 ? "4:2:0" : chroma == 2 ? "4:2:2" : chroma == 3 ? "4:4:4" : "mono")) 位深=\(bdL)/\(bdC)")
            }
        } else { print("  无 hvcC atom") }
        for k in ext.keys.sorted() where k.contains("ColorPrimaries") || k.contains("TransferFunction") || k.contains("MatrixCoefficients") {
            print("  \(k)=\(ext[k]!)")
        }
    }

    for (label, spec) in [("强制硬解", [kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder as String: true, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String: true]),
                          ("允许软解", [String: Any]())] {
        var s: VTDecompressionSession?
        let dst = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
        let st = VTDecompressionSessionCreate(allocator: nil, formatDescription: fd, decoderSpecification: spec as CFDictionary, imageBufferAttributes: dst, outputCallback: nil, decompressionSessionOut: &s)
        print("  \(label): \(st == noErr ? "OK" : "失败 \(st)")")
        if let s = s { VTDecompressionSessionInvalidate(s) }
    }
    print("")
}
