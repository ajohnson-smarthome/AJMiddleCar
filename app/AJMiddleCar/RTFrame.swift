import Foundation

/// The real-time wire, as text. Pure — no Network, no I/O — so it is host-tested with `swiftc`.
///
/// Every constant and every field name that the schema knows about comes from `CarContract` and
/// `TelemetryKey`, which are generated from `contract/car-api.json`. Four hand-written copies of
/// one protocol is what the generator exists to prevent, so a literal in this file is a bug —
/// with one exception, recorded below.
///
/// **The exception:** the schema names the *values* `proto` and `device` but not the JSON keys
/// that carry them, and it names no key at all for the command's `t`/`y`. Those five spellings
/// are literals here, in one place, and nowhere else in the app.
enum RTFrame {
    private static let protoKey = "proto"
    private static let deviceKey = "device"
    private static let fwKey = "fw"
    private static let throttleKey = "t"
    private static let yawKey = "y"

    // MARK: - app → car

    /// Opens a session. Repeated until the car answers; the reply carries the car's identity.
    static func hello(sid: String) -> String {
        "{\"\(protoKey)\":\(CarContract.proto),\"\(CarContract.helloField)\":\"\(sid)\"}"
    }

    /// One 10 Hz command. `String(format:)` with no locale formats in the C locale, so a phone
    /// set to a comma-decimal language cannot emit `{"t":0,50}` and desync the car's parser.
    static func command(seq: UInt32, t: Double, y: Double) -> String {
        String(format: "{\"%@\":%u,\"%@\":%.2f,\"%@\":%.2f}",
               CarContract.seqField, seq, throttleKey, clamp(t), yawKey, clamp(y))
    }

    /// A deliberate stop. Distinct from silence: it suppresses the car's retreat and drops
    /// ownership, so the car stops where it stands instead of retracing its path back to us.
    static func bye(seq: UInt32) -> String {
        String(format: "{\"%@\":%u,\"%@\":%.2f,\"%@\":%.2f,\"%@\":1}",
               CarContract.seqField, seq, throttleKey, 0.0, yawKey, 0.0, CarContract.byeField)
    }

    /// `seq` is a `uint32` the car compares as `(int32_t)(seq - last) > 0`, so wrapping past
    /// `UInt32.max` is correct on both sides and needs no reset.
    static func nextSeq(_ seq: UInt32) -> UInt32 { seq &+ 1 }

    /// A per-session id: 8 hex characters, as the contract's `hello` field specifies.
    static func sessionID(_ value: UInt32 = .random(in: .min ... .max)) -> String {
        String(format: "%08x", value)
    }

    private static func clamp(_ v: Double) -> Double { Swift.min(1, Swift.max(-1, v)) }

    // MARK: - car → app

    enum Inbound: Equatable {
        /// The car adopted us. Identity arrives here rather than from a separate `/status` probe.
        case helloReply(sid: String, device: String, fw: String)
        case telemetry(Telemetry)
    }

    /// Classify one datagram. Returns nil for anything that is not JSON or carries neither a
    /// hello reply nor a recognisable telemetry frame — a stray datagram must not disturb a
    /// live session.
    static func parse(_ text: String) -> Inbound? {
        guard let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }

        if let sid = j[CarContract.helloField] as? String {
            // An unknown protocol is refused by name; parsing it as if it were ours is how a
            // version mismatch turns into an unexplainable bug.
            guard (j[protoKey] as? Int) == CarContract.proto else { return nil }
            return .helloReply(sid: sid,
                               device: j[deviceKey] as? String ?? "",
                               fw: j[fwKey] as? String ?? "")
        }
        guard let t = Telemetry(json: j) else { return nil }
        return .telemetry(t)
    }
}

/// One telemetry frame, 5 Hz from the car. Fields are exactly `TelemetryKey`.
struct Telemetry: Equatable, Sendable {
    var seq: Int?
    var rxFps: Int?
    var rssi: Int?
    var wdtTrips: Int?
    var uptimeS: Int?
    var heap: Int?
    var calibrated: Bool?
    var busOk: Bool?
    /// Which source owns the actuator (`rt` / `console` / `calib` / `recover` / `ota` / `safe` /
    /// `none`). The app can be driving and *not* be the one the car is listening to.
    var ctl: String?

    init() {}

    /// nil unless the object looks like telemetry at all — otherwise every malformed datagram
    /// would present as a frame with all fields missing and keep the link looking alive.
    init?(json j: [String: Any]) {
        guard j[TelemetryKey.uptimeS] != nil || j[TelemetryKey.rxFps] != nil
                || j[TelemetryKey.calibrated] != nil else { return nil }
        seq = j[TelemetryKey.seq] as? Int
        rxFps = j[TelemetryKey.rxFps] as? Int
        // 0 means "unavailable" on the car's side, not "very strong".
        if let r = j[TelemetryKey.rssi] as? Int, r != 0 { rssi = r }
        wdtTrips = j[TelemetryKey.wdtTrips] as? Int
        uptimeS = j[TelemetryKey.uptimeS] as? Int
        heap = j[TelemetryKey.heap] as? Int
        calibrated = j[TelemetryKey.calibrated] as? Bool
        busOk = j[TelemetryKey.busOk] as? Bool
        ctl = j[TelemetryKey.ctl] as? String
    }

    static func parse(_ text: String) -> Telemetry? {
        guard let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return Telemetry(json: j)
    }
}
