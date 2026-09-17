import Foundation

/// The real-time wire, as text. Pure — no Network, no I/O — so it is host-tested with `swiftc`.
///
/// Every constant and every key comes from `CarContract` / `RTType`, generated from
/// `contract/car-api.json`, and the shapes the car sends decode into the generated `DeviceInfo`
/// and `Telemetry`. Four hand-written copies of one protocol is what the generator exists to
/// prevent, so a string literal on the wire path in this file is a bug.
enum RTFrame {

    // MARK: - app → car

    /// Opens a session. Repeated until the car answers; the reply carries the car's identity.
    static func hello(sid: String) -> String {
        "{\"\(CarContract.protoField)\":\(CarContract.proto),"
        + "\"\(CarContract.typeField)\":\"\(RTType.hello)\","
        + "\"\(CarContract.sessionField)\":\"\(sid)\"}"
    }

    /// Subscribes to the video stream — on the video port, not the control port. Hello-shaped:
    /// the car accepts it only from the session's owner. `key` asks for a keyframe now.
    static func view(sid: String, key: Bool = false) -> String {
        "{\"\(CarContract.protoField)\":\(CarContract.proto),"
        + "\"\(CarContract.typeField)\":\"\(RTType.view)\","
        + "\"\(CarContract.sessionField)\":\"\(sid)\""
        + (key ? ",\"\(CarContract.keyField)\":true" : "")
        + "}"
    }

    /// One 10 Hz drive. `String(format:)` with no locale formats in the C locale, so a phone
    /// set to a comma-decimal language cannot emit `0,50` and desync the car's parser.
    static func command(seq: UInt32, throttle: Double, turn: Double) -> String {
        String(format: "{\"%@\":%d,\"%@\":\"%@\",\"%@\":%u,\"%@\":%.2f,\"%@\":%.2f}",
               CarContract.protoField, CarContract.proto,
               CarContract.typeField, RTType.drive,
               CarContract.seqField, seq,
               CarContract.throttleField, clamp(throttle),
               CarContract.turnField, clamp(turn))
    }

    /// A deliberate stop. Distinct from silence: it suppresses the car's retreat and drops
    /// ownership, so the car stops where it stands instead of retracing its path back to us.
    static func bye(seq: UInt32) -> String {
        String(format: "{\"%@\":%d,\"%@\":\"%@\",\"%@\":%u}",
               CarContract.protoField, CarContract.proto,
               CarContract.typeField, RTType.bye,
               CarContract.seqField, seq)
    }

    /// `seq` is a `uint32` the car compares as `(int32_t)(seq - last) > 0`, so wrapping past
    /// `UInt32.max` is correct on both sides and needs no reset.
    static func nextSeq(_ seq: UInt32) -> UInt32 { seq &+ 1 }

    /// The same comparison, for the counter the car puts in *its* telemetry.
    static func seqNewer(_ seq: Int, than last: Int) -> Bool {
        let delta = UInt32(truncatingIfNeeded: seq) &- UInt32(truncatingIfNeeded: last)
        return Int32(bitPattern: delta) > 0
    }

    /// A per-session id: 8 hex characters, as the contract's `session` field specifies.
    static func sessionID(_ value: UInt32 = .random(in: .min ... .max)) -> String {
        String(format: "%08x", value)
    }

    /// Non-finite input is a stop, not a direction: max(-1, .nan) is -1, so an unguarded NaN
    /// would stream as sustained full reverse — and formatted raw it would not even be JSON.
    private static func clamp(_ v: Double) -> Double {
        v.isFinite ? Swift.min(1, Swift.max(-1, v)) : 0
    }

    // MARK: - car → app

    enum Inbound: Equatable {
        /// The car adopted us. Its identity arrives here, the same `device` group `/status` carries.
        case helloReply(sid: String, device: DeviceInfo)
        case telemetry(Telemetry)
    }

    private struct HelloAck: Decodable { let device: DeviceInfo }

    /// Classify one datagram by its `type`. Returns nil for anything that is not JSON, has no
    /// type, is a type the car does not send, or does not carry what its type needs — a stray
    /// datagram must not disturb a live session.
    static func parse(_ text: String) -> Inbound? {
        guard let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = j[CarContract.typeField] as? String else { return nil }
        switch type {
        case RTType.helloAck:
            guard let sid = j[CarContract.sessionField] as? String,
                  let ack = try? JSONDecoder().decode(HelloAck.self, from: data) else { return nil }
            // proto is on the wire but not judged here — see spec 2026-09-17-proto-out-of-app.
            return .helloReply(sid: sid, device: ack.device)
        case RTType.telemetry:
            guard let t = try? JSONDecoder().decode(Telemetry.self, from: data) else { return nil }
            return .telemetry(t)
        default:
            return nil
        }
    }
}
