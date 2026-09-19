import AVFoundation
import SwiftUI
import UIKit

/// The picture. An `AVSampleBufferDisplayLayer` fed straight from the video queue: each
/// complete frame becomes one `CMSampleBuffer` marked "display immediately", so the only
/// queue between the network and the glass is the decoder's own. No `controlTimebase` —
/// it is incompatible with immediate display, and we have no clock in common with the car.
///
/// Rotation and mirroring come from the contract and are a transform on the layer: turning
/// a RAW sensor over on the car would shift its Bayer grid and need the ISP retuned, while
/// here it costs nothing.
struct VideoView: UIViewRepresentable {
    let link: VideoLink

    func makeUIView(context: Context) -> VideoLayerView {
        let v = VideoLayerView()
        v.backgroundColor = .clear
        v.layer.transform = Self.transform
        context.coordinator.attach(v.displayLayer, link: link)
        return v
    }

    func updateUIView(_ uiView: VideoLayerView, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator() }

    static func dismantleUIView(_ uiView: VideoLayerView, coordinator: Coordinator) {
        coordinator.detach()
    }

    private static var transform: CATransform3D {
        var t = CATransform3DIdentity
        if CarContract.videoRotation == 180 { t = CATransform3DRotate(t, .pi, 0, 0, 1) }
        if CarContract.videoMirror { t = CATransform3DScale(t, -1, 1, 1) }
        return t
    }

    /// Lives on the video queue after `attach`: `onFrame` calls it there, the renderer takes
    /// samples from any thread, and `detach` tears the fields down there too — behind the
    /// closure removal, so an in-flight `show` finishes before they go. `attach` writes them
    /// on main, but before the `onFrame` assignment, whose setter is a `queue.async`: nothing
    /// on the queue can observe them half-built.
    final class Coordinator {
        private var renderer: AVSampleBufferVideoRenderer?
        private var format: CMFormatDescription?
        private weak var link: VideoLink?

        func attach(_ layer: AVSampleBufferDisplayLayer, link: VideoLink) {
            // Fill, not fit: the drive screen frames this view as a 16:9 window and the frame
            // is already 16:9 (the car cropped the sensor's 4:3 to it), so on a 16:9 window
            // nothing is trimmed; a squatter screen loses the sides — see `DriveLayout`.
            layer.videoGravity = .resizeAspectFill
            renderer = layer.sampleBufferRenderer
            self.link = link
            link.onFrame = { [weak self] frame, isKey in self?.show(frame, isKey: isKey) }
        }

        func detach() {
            guard let link else { return }
            link.onFrame = nil
            // The removal above is a `queue.async` store; the same queue, behind it, is the one
            // place where no `show` can still be reading these.
            link.queue.async { [self] in
                renderer?.flush()
                renderer = nil
                format = nil
            }
        }

        private func show(_ frame: Data, isKey: Bool) {
            guard let renderer else { return }
            // The decoder was taken away (background) or choked: flush, and start over from
            // a keyframe — the next sample after a flush has to be an IDR.
            if renderer.status == .failed || renderer.requiresFlushToResumeDecoding {
                renderer.flush()
                format = nil
                Task { @MainActor [link] in link?.requestKeyframe() }
            }
            let split = AnnexB.split(frame)
            if let sps = split.sps, let pps = split.pps {
                format = Self.makeFormat(sps: sps, pps: pps)
            }
            // No format yet (fresh, or just flushed) means no keyframe yet: a P-frame here has
            // nothing to predict from, and the receiver only hands over P-frames after an IDR
            // anyway — this guard covers the flush case, where the receiver does not know.
            guard let format, !split.avcc.isEmpty else { return }
            guard let sample = Self.makeSample(split.avcc, format: format) else { return }
            renderer.enqueue(sample)
        }

        private static func makeFormat(sps: Data, pps: Data) -> CMFormatDescription? {
            var desc: CMFormatDescription?
            sps.withUnsafeBytes { s in
                pps.withUnsafeBytes { p in
                    let sets = [s.bindMemory(to: UInt8.self).baseAddress!, p.bindMemory(to: UInt8.self).baseAddress!]
                    let sizes = [sps.count, pps.count]
                    CMVideoFormatDescriptionCreateFromH264ParameterSets(
                        allocator: kCFAllocatorDefault, parameterSetCount: 2, parameterSetPointers: sets,
                        parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &desc)
                }
            }
            return desc
        }

        private static func makeSample(_ avcc: Data, format: CMFormatDescription) -> CMSampleBuffer? {
            var block: CMBlockBuffer?
            guard CMBlockBufferCreateWithMemoryBlock(
                    allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count,
                    blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
                    dataLength: avcc.count, flags: 0, blockBufferOut: &block) == noErr, let block else { return nil }
            let copied = avcc.withUnsafeBytes { CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: avcc.count) }
            guard copied == noErr else { return nil }
            var sample: CMSampleBuffer?
            var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: .invalid, decodeTimeStamp: .invalid)
            var length = avcc.count
            guard CMSampleBufferCreateReady(
                    allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
                    sampleCount: 1, sampleTimingEntryCount: 1, sampleTimingArray: &timing,
                    sampleSizeEntryCount: 1, sampleSizeArray: &length, sampleBufferOut: &sample) == noErr,
                  let sample else { return nil }
            // The attachment is set on the sample's attachments array, not on the buffer.
            if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
               CFArrayGetCount(attachments) > 0 {
                let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
                CFDictionarySetValue(dict, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                     Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
            }
            return sample
        }
    }
}

final class VideoLayerView: UIView {
    override class var layerClass: AnyClass { AVSampleBufferDisplayLayer.self }
    var displayLayer: AVSampleBufferDisplayLayer { layer as! AVSampleBufferDisplayLayer }
}
