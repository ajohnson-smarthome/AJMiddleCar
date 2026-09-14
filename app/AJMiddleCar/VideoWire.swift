import Foundation

/// The video datagram, as `firmware/car/core/main/video_wire.h` spells it: a 12-byte
/// big-endian header and up to `videoChunkBytes` of an H.264 Annex B frame. The receiver
/// implements the same rules, in the same order, as the C and Python receivers; the tests
/// under `app/tests/videowire` are the C test's scenarios, so the three cannot drift apart
/// without one of them going red.
struct VideoHeader: Equatable {
    static let keyFlag: UInt8 = 0x01

    var proto: UInt8
    var flags: UInt8
    var stream: UInt8
    var frame: UInt16
    var chunk: UInt8
    var count: UInt8
    var capturedMs: UInt32

    var isKeyframe: Bool { flags & Self.keyFlag != 0 }

    func pack() -> Data {
        var d = Data(capacity: CarContract.videoHeaderBytes)
        d.append(contentsOf: [proto, flags, stream, 0, UInt8(frame >> 8), UInt8(frame & 0xff), chunk, count,
                              UInt8(capturedMs >> 24), UInt8((capturedMs >> 16) & 0xff),
                              UInt8((capturedMs >> 8) & 0xff), UInt8(capturedMs & 0xff)])
        return d
    }

    /// nil: not a header this receiver accepts. Strict on purpose — a set reserved byte or
    /// an undefined flag is a format nobody agreed on, and a new format gets a new proto.
    init?(data: Data) {
        guard data.count >= CarContract.videoHeaderBytes else { return nil }
        let b = [UInt8](data.prefix(CarContract.videoHeaderBytes))
        guard b[0] == UInt8(CarContract.videoWireProto), b[1] & ~Self.keyFlag == 0, b[3] == 0,
              b[7] != 0, b[6] < b[7] else { return nil }
        proto = b[0]; flags = b[1]; stream = b[2]
        frame = UInt16(b[4]) << 8 | UInt16(b[5])
        chunk = b[6]; count = b[7]
        capturedMs = UInt32(b[8]) << 24 | UInt32(b[9]) << 16 | UInt32(b[10]) << 8 | UInt32(b[11])
    }

    init(proto: UInt8, flags: UInt8, stream: UInt8, frame: UInt16, chunk: UInt8, count: UInt8, capturedMs: UInt32) {
        self.proto = proto; self.flags = flags; self.stream = stream; self.frame = frame
        self.chunk = chunk; self.count = count; self.capturedMs = capturedMs
    }
}

enum VideoWire {
    /// RFC 1982 on 16 bits: (Int16)(a - b) > 0. At 15 fps the counter wraps in 73 minutes.
    static func frameNewer(_ a: UInt16, _ b: UInt16) -> Bool {
        Int16(bitPattern: a &- b) > 0
    }

    static func chunkCount(_ n: Int) -> Int {
        guard n > 0 else { return 0 }
        let c = (n + CarContract.videoChunkBytes - 1) / CarContract.videoChunkBytes
        return c > 255 ? 0 : c
    }

    /// Every datagram of one frame, in order. For the tests and the mock's twin; the app
    /// only receives.
    static func chunks(payload: Data, stream: UInt8, frame: UInt16, keyframe: Bool, capturedMs: UInt32) -> [Data] {
        let n = chunkCount(payload.count)
        let C = CarContract.videoChunkBytes
        return (0..<n).map { i in
            let h = VideoHeader(proto: UInt8(CarContract.videoWireProto), flags: keyframe ? VideoHeader.keyFlag : 0,
                                stream: stream, frame: frame, chunk: UInt8(i), count: UInt8(n), capturedMs: capturedMs)
            let lo = payload.startIndex + i * C
            let hi = min(lo + C, payload.endIndex)
            return h.pack() + payload[lo..<hi]
        }
    }
}

/// Reassembles frames from datagrams. Not thread-safe: one queue feeds it.
final class VideoReceiver {
    enum Event: Equatable {
        case none                       // placed, ignored (older, duplicate), or withheld after a loss
        case frame(Data, keyframe: Bool)
        case loss                       // a frame was abandoned or a chunk was corrupt: ask for a keyframe
        case bad                        // not a datagram this receiver accepts
    }

    private(set) var dropped = 0
    private(set) var bad = 0

    private let cap: Int
    private var stream: UInt8?
    private var last: UInt16?                  // newest frame finished or abandoned
    private var cur: (frame: UInt16, count: Int, flags: UInt8, parts: [Int: Data])?
    private var waitKey = true

    init(cap: Int = 255 * CarContract.videoChunkBytes) { self.cap = cap }

    private func abandon() {
        last = cur?.frame
        cur = nil
        waitKey = true
        dropped += 1
    }

    func feed(_ datagram: Data) -> Event {
        guard let h = VideoHeader(data: datagram) else { bad += 1; return .bad }
        let body = datagram.dropFirst(CarContract.videoHeaderBytes)
        let isLast = Int(h.chunk) == Int(h.count) - 1
        guard !body.isEmpty, body.count <= CarContract.videoChunkBytes,
              isLast || body.count == CarContract.videoChunkBytes else { bad += 1; return .bad }

        if stream != h.stream {
            stream = h.stream; cur = nil; last = nil; waitKey = true
        }
        var ev = Event.none
        if let c = cur, c.frame != h.frame {
            guard VideoWire.frameNewer(h.frame, c.frame) else { return .none }
            abandon()
            ev = .loss
        }
        if cur == nil {
            if let l = last, !VideoWire.frameNewer(h.frame, l) { return ev }
            cur = (h.frame, Int(h.count), h.flags, [:])
            if Int(h.count) * CarContract.videoChunkBytes > cap { abandon(); return .loss }
        } else if cur!.count != Int(h.count) || cur!.flags != h.flags {
            abandon()
            return .loss
        }
        if cur!.parts[Int(h.chunk)] != nil { return ev }
        cur!.parts[Int(h.chunk)] = Data(body)
        guard cur!.parts.count == cur!.count else { return ev }

        let done = cur!
        cur = nil
        last = done.frame
        let key = done.flags & VideoHeader.keyFlag != 0
        if key { waitKey = false }
        if waitKey { return ev }
        var out = Data()
        for i in 0..<done.count { out.append(done.parts[i]!) }
        return .frame(out, keyframe: key)
    }
}

/// Annex B (start codes) -> AVCC (4-byte lengths), the shape VideoToolbox wants. SPS and
/// PPS come out separately — they go into the format description, never into a sample —
/// and access-unit delimiters are dropped.
enum AnnexB {
    struct Split: Equatable {
        var avcc = Data()
        var sps: Data?
        var pps: Data?
        var isIDR = false
    }

    static func split(_ frame: Data) -> Split {
        var out = Split()
        let b = [UInt8](frame)
        var starts: [Int] = []          // index of each NAL's first byte
        var i = 0
        while i + 2 < b.count {
            if b[i] == 0, b[i + 1] == 0 {
                if b[i + 2] == 1 { starts.append(i + 3); i += 3; continue }
                if b[i + 2] == 0, i + 3 < b.count, b[i + 3] == 1 { starts.append(i + 4); i += 4; continue }
            }
            i += 1
        }
        for (k, s) in starts.enumerated() {
            var e = k + 1 < starts.count ? starts[k + 1] : b.count
            // Trim the next start code (and a leading zero of a 4-byte one) off this NAL.
            if k + 1 < starts.count {
                e -= 3
                if e > s, b[e - 1] == 0 { e -= 1 }
            }
            guard e > s else { continue }
            let nal = Data(b[s..<e])
            switch nal[nal.startIndex] & 0x1f {
            case 7: out.sps = nal
            case 8: out.pps = nal
            case 9: break
            default:
                if nal[nal.startIndex] & 0x1f == 5 { out.isIDR = true }
                let n = UInt32(nal.count)
                out.avcc.append(contentsOf: [UInt8(n >> 24), UInt8((n >> 16) & 0xff), UInt8((n >> 8) & 0xff), UInt8(n & 0xff)])
                out.avcc.append(nal)
            }
        }
        return out
    }
}
