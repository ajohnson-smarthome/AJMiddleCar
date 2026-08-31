import Foundation

/// Decodes the dongle's own `GET /status` and `GET /net` bodies. Pure — no `Network`, no I/O —
/// so it is host-tested with `swiftc`. Every wire key comes from `DongleStatusKey` /
/// `DongleContract`, which are generated from `contract/dongle-api.json`: a `CodingKeys` raw
/// value must be a compile-time literal, which would force retyping those names here, so
/// decoding instead goes through this file-local key built from the generated constant's
/// runtime value.
private struct DongleKey: CodingKey {
    let stringValue: String
    init(_ value: String) { stringValue = value }
    init?(stringValue: String) { self.stringValue = stringValue }
    var intValue: Int? { nil }
    init?(intValue: Int) { nil }
}

/// A `/status` snapshot. Every field is required — a document missing one throws rather than
/// decoding with a default, because a dongle that answers a truncated document is a dongle the
/// app must not believe.
public struct DongleStatus: Decodable {
    public let device: String
    public let fw: String
    public let idf: String
    public let usb: String
    /// Distinguishes "never updated" from "the update was reverted" — the bootloader's own
    /// verdict on the last OTA, not something the app can infer any other way.
    public let rollback: Bool
    public let net: DongleNetStatus

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DongleKey.self)
        device = try c.decode(String.self, forKey: DongleKey(DongleStatusKey.device))
        fw = try c.decode(String.self, forKey: DongleKey(DongleStatusKey.fw))
        idf = try c.decode(String.self, forKey: DongleKey(DongleStatusKey.idf))
        usb = try c.decode(String.self, forKey: DongleKey(DongleStatusKey.usb))
        rollback = try c.decode(Bool.self, forKey: DongleKey(DongleStatusKey.rollback))
        net = try c.decode(DongleNetStatus.self, forKey: DongleKey(DongleStatusKey.net))
    }

    public static func parse(_ data: Data) throws -> DongleStatus {
        try JSONDecoder().decode(DongleStatus.self, from: data)
    }
}

/// The `net` object nested inside `/status`: the dongle's own view of its station link.
public struct DongleNetStatus: Decodable, Equatable {
    public let ssid: String
    public let state: State
    public let rssi: Int

    /// The contract's four states, plus `unknown` — so a firmware that grows a fifth state does
    /// not make the app fail to parse a document it otherwise understands.
    public enum State: Equatable, Decodable {
        case idle
        case joining
        case connected
        case failed
        case unknown(String)

        public init(from decoder: Decoder) throws {
            let raw = try decoder.singleValueContainer().decode(String.self)
            switch raw {
            case DongleNetState.idle: self = .idle
            case DongleNetState.joining: self = .joining
            case DongleNetState.connected: self = .connected
            case DongleNetState.failed: self = .failed
            default: self = .unknown(raw)
            }
        }
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DongleKey.self)
        ssid = try c.decode(String.self, forKey: DongleKey(DongleStatusKey.netSsid))
        state = try c.decode(State.self, forKey: DongleKey(DongleStatusKey.netState))
        rssi = try c.decode(Int.self, forKey: DongleKey(DongleStatusKey.netRssi))
    }
}

/// `GET /net`'s body: `ssid` and `configured` only. The dongle never sends a password, and this
/// type has nowhere to put one — if a future firmware slipped a `password` key into the reply,
/// decoding only ever reads the two keys below, so nothing here would store it.
public struct DongleNet: Decodable {
    public let ssid: String
    public let configured: Bool

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DongleKey.self)
        ssid = try c.decode(String.self, forKey: DongleKey(DongleContract.ssidField))
        configured = try c.decode(Bool.self, forKey: DongleKey(DongleContract.configuredField))
    }

    public static func parse(_ data: Data) throws -> DongleNet {
        try JSONDecoder().decode(DongleNet.self, from: data)
    }
}
