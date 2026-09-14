// Host test for the video datagram. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}
func hex(_ s: String) -> Data {
    var d = Data(); var i = s.startIndex
    while i < s.endIndex { let j = s.index(i, offsetBy: 2); d.append(UInt8(s[i..<j], radix: 16)!); i = j }
    return d
}
func payload(_ n: Int, _ seed: Int = 1) -> Data { Data((0..<n).map { UInt8((seed + $0 * 7) & 0xFF) }) }

// The header vectors, as contract/car-api.json's video.vectors spell them.
let keyFirst = VideoHeader(proto: 1, flags: 1, stream: 3, frame: 7, chunk: 0, count: 30, capturedMs: 812345)
check(keyFirst.pack() == hex("010103000007001e000c6539"), "pack keyframe first chunk")
check(VideoHeader(data: hex("010103000007001e000c6539")) == keyFirst, "unpack keyframe first chunk")
let edge = VideoHeader(proto: 1, flags: 0, stream: 255, frame: 65535, chunk: 2, count: 3, capturedMs: 4294967295)
check(edge.pack() == hex("0100ff00ffff0203ffffffff"), "pack edge")
check(VideoHeader(data: hex("0100ff00ffff0203ffffffff")) == edge, "unpack edge")
for (name, bad) in [("foreign proto", "020103000007001e000c6539"), ("chunk not below count", "0101030000071e1e000c6539"),
                    ("count zero", "010103000007000000000000"), ("reserved byte set", "010103010007001e000c6539"),
                    ("unknown flag", "010203000007001e000c6539")] {
    check(VideoHeader(data: hex(bad)) == nil, "reject \(name)")
}
check(VideoHeader(data: hex("010103000007001e000c65")) == nil, "reject short")

check(VideoWire.frameNewer(1, 0) && VideoWire.frameNewer(0, 65535) && VideoWire.frameNewer(32768, 1), "newer")
check(!VideoWire.frameNewer(65535, 0) && !VideoWire.frameNewer(5, 5) && !VideoWire.frameNewer(1, 32768), "not newer")

let C = CarContract.videoChunkBytes, H = CarContract.videoHeaderBytes
check([0, 1, C, C + 1, C * 255, C * 255 + 1].map(VideoWire.chunkCount) == [0, 1, 1, 2, 255, 0], "chunkCount")
let ds = VideoWire.chunks(payload: payload(3000), stream: 1, frame: 9, keyframe: true, capturedMs: 5)
check(ds.map(\.count) == [H + C, H + C, H + 3000 - 2 * C], "chunk sizes")
check(VideoHeader(data: ds[0])?.count == 3 && VideoHeader(data: ds[2])?.chunk == 2, "chunk headers")
check(ds.map { $0.dropFirst(H) }.reduce(Data(), +) == payload(3000), "chunk payloads")

// The receiver, on the C test's scenarios.
func send(_ rx: VideoReceiver, _ stream: UInt8, _ frame: UInt16, _ key: Bool, _ data: Data,
          order: [Int]? = nil, skip: Int? = nil) -> VideoReceiver.Event {
    let ds = VideoWire.chunks(payload: data, stream: stream, frame: frame, keyframe: key, capturedMs: 1000)
    var ev = VideoReceiver.Event.none
    for i in order ?? Array(ds.indices) where i != skip { ev = rx.feed(ds[i]) }
    return ev
}
let rx = VideoReceiver()
check(send(rx, 1, 0, false, payload(3000)) == .none, "P before any key is withheld")
check(send(rx, 1, 1, true, payload(3000, 42)) == .frame(payload(3000, 42), keyframe: true), "keyframe delivered")
check(send(rx, 1, 2, false, payload(2900, 7), order: [2, 0, 1]) == .frame(payload(2900, 7), keyframe: false), "reordered")
check(send(rx, 1, 3, false, payload(3000), order: [0, 1, 1, 2]) == .frame(payload(3000), keyframe: false), "duplicate")
check(send(rx, 1, 4, false, payload(3000), skip: 1) == .none, "incomplete stays quiet")
check(send(rx, 1, 5, false, payload(3000), order: [0]) == .loss, "the next frame reports the loss")
check(rx.dropped == 1, "dropped 1")
check(send(rx, 1, 5, false, payload(3000), order: [1, 2]) == .none, "withheld after loss")
check(send(rx, 1, 6, false, payload(3000)) == .none, "still withheld")
check(send(rx, 1, 7, true, payload(3000)) == .frame(payload(3000), keyframe: true), "the key unlocks")
check(send(rx, 1, 8, false, payload(3000)) == .frame(payload(3000), keyframe: false), "P after key")
check(send(rx, 1, 7, true, payload(3000), order: [0]) == .none, "straggler ignored")
check(send(rx, 1, 9, false, payload(3000)) == .frame(payload(3000), keyframe: false), "after straggler")
check(send(rx, 1, 10, false, payload(3000), skip: 2) == .none, "lost again")
check(send(rx, 1, 11, true, payload(100)) == .frame(payload(100), keyframe: true), "one-chunk key after loss")
check(rx.dropped == 2, "dropped 2")
let rx2 = VideoReceiver()
check(send(rx2, 2, 65535, true, payload(3000)) == .frame(payload(3000), keyframe: true), "edge key")
check(send(rx2, 2, 0, false, payload(3000)) == .frame(payload(3000), keyframe: false), "wrap")
check(send(rx2, 3, 0, false, payload(3000)) == .none, "new stream waits")
check(send(rx2, 3, 1, true, payload(3000)) == .frame(payload(3000), keyframe: true), "new stream key")
check(send(rx2, 3, 2, false, payload(3000), order: [0]) == .none, "one chunk of frame 2")
let badCount = VideoHeader(proto: 1, flags: 0, stream: 3, frame: 2, chunk: 1, count: 4, capturedMs: 0).pack() + Data(count: C)
check(rx2.feed(badCount) == .loss, "count disagrees")
let mid = VideoHeader(proto: 1, flags: 0, stream: 3, frame: 3, chunk: 0, count: 3, capturedMs: 0).pack()
check(rx2.feed(mid + Data(count: 100)) == .bad && rx2.feed(mid) == .bad && rx2.feed(mid + Data(count: C + 1)) == .bad, "length invariant")
check(rx2.feed(hex("020103000007001e000c6539") + Data(count: C)) == .bad, "bad header")
let small = VideoReceiver(cap: 5000)
check(small.feed(VideoHeader(proto: 1, flags: 1, stream: 1, frame: 0, chunk: 0, count: 200, capturedMs: 0).pack() + Data(count: C)) == .loss, "too big")
check(small.dropped == 1, "too big counts")

// R7(a): a stream change while a frame is in progress discards the abandoned frame
// silently (rule 3) — no .loss for the stray stream-1 chunk, dropped unchanged, and the
// stream-2 keyframe that follows is still delivered whole.
let rxA = VideoReceiver()
check(send(rxA, 1, 0, true, payload(3000)) == .frame(payload(3000), keyframe: true), "R7a: stream 1 keyframe delivered")
check(send(rxA, 1, 1, false, payload(3000), order: [0]) == .none, "R7a: stream 1 frame 1 chunk 0 only, in progress")
check(send(rxA, 2, 0, true, payload(3000, 9)) == .frame(payload(3000, 9), keyframe: true), "R7a: stream 2 keyframe complete despite stream 1 in progress")
check(rxA.dropped == 0, "R7a: stream change is a reset, not a loss")

// R7(b): a flags mismatch mid-frame (same frame, same count) is corruption — reported as
// .loss with dropped incrementing.
let rxB = VideoReceiver()
check(send(rxB, 1, 0, true, payload(3000)) == .frame(payload(3000), keyframe: true), "R7b: keyframe delivered")
let dsB = VideoWire.chunks(payload: payload(3000, 7), stream: 1, frame: 1, keyframe: false, capturedMs: 1001)
check(rxB.feed(dsB[0]) == .none, "R7b: frame 1 chunk 0, flags 0, count 3, in progress")
let mismatched = VideoHeader(proto: UInt8(CarContract.videoWireProto), flags: VideoHeader.keyFlag, stream: 1,
                              frame: 1, chunk: 1, count: UInt8(dsB.count), capturedMs: 1001).pack() + dsB[1].dropFirst(H)
check(rxB.feed(mismatched) == .loss, "R7b: flags mismatch mid-frame is loss")
check(rxB.dropped == 1, "R7b: dropped +1")

// R7(c): a bad header and a straggler from an already-delivered frame must not reset
// wait_key or the frame bookkeeping — the next in-order P-frame still delivers.
let rxC = VideoReceiver()
check(send(rxC, 1, 0, true, payload(3000)) == .frame(payload(3000), keyframe: true), "R7c: keyframe delivered")
check(send(rxC, 1, 1, false, payload(3000)) == .frame(payload(3000), keyframe: false), "R7c: frame 1 delivered")
let foreign = VideoHeader(proto: 0, flags: 0, stream: 1, frame: 2, chunk: 0, count: 3, capturedMs: 0).pack() + Data(count: C)
check(rxC.feed(foreign) == .bad, "R7c: bad header (foreign proto) touches nothing")
let straggler = VideoWire.chunks(payload: payload(3000), stream: 1, frame: 1, keyframe: false, capturedMs: 1001)[0]
check(rxC.feed(straggler) == .none, "R7c: straggler of already-delivered frame is ignored")
check(send(rxC, 1, 2, false, payload(3000)) == .frame(payload(3000), keyframe: false), "R7c: next in-order P-frame still delivers")
check(rxC.dropped == 0, "R7c: no drop")

// Annex B -> AVCC: SPS/PPS out, AUD dropped, VCL and SEI length-prefixed.
let sps = Data([0x67, 0x42, 0x00, 0x1f]), pps = Data([0x68, 0xce, 0x38, 0x80])
let idr = Data([0x65, 0x88, 0x84, 0x00]), sei = Data([0x06, 0x05, 0x01]), aud = Data([0x09, 0xf0])
let sc4 = Data([0, 0, 0, 1]), sc3 = Data([0, 0, 1])
let key = sc4 + aud + sc4 + sps + sc4 + pps + sc3 + sei + sc4 + idr
let s = AnnexB.split(key)
check(s.sps == sps && s.pps == pps && s.isIDR, "sps/pps out, idr flagged")
check(s.avcc == Data([0, 0, 0, 3]) + sei + Data([0, 0, 0, 4]) + idr, "avcc keeps sei and vcl, drops aud")
let p = AnnexB.split(sc4 + Data([0x41, 0x9a, 0x02]))
check(p.sps == nil && p.pps == nil && !p.isIDR && p.avcc == Data([0, 0, 0, 3, 0x41, 0x9a, 0x02]), "p-frame")
check(AnnexB.split(Data()).avcc.isEmpty, "empty")

if failures == 0 { print("test_videowire: OK") } else { exit(1) }
