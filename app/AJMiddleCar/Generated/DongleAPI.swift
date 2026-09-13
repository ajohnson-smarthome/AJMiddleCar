// generated from contract/dongle-api.json by tools/gen_contract.py - do not edit

public enum DongleContract {
    public static let proto = 1
    public static let device = "ajdongle"
    public static let host = "192.168.7.1"
    public static let port: UInt16 = 8080
    public static let relayHttpPort: UInt16 = 80
    public static let relayRtPort: UInt16 = 4210

    public static let statusPath = "/status"
    public static let wifiPath = "/wifi"
    public static let otaPath = "/ota"

    public static let ssidMin = 1
    public static let ssidMax = 32
    public static let passMin = 8
    public static let passMax = 63

    public static let ssidField = "ssid"
    public static let passwordField = "password"
}

/// Who is answering.
public struct DongleDevice: Codable, Equatable, Sendable {
    /// the device name; ajdongle for this adapter
    public var id: String
    /// firmware version, v<semver>+<build>
    public var fw: String
    /// the number after + in fw, as an integer
    public var build: Int
    /// the bootloader reverted the last update
    public var rolled_back: Bool
    /// the ESP-IDF version this image was built with
    public var idf: String
    public init(id: String, fw: String, build: Int, rolled_back: Bool, idf: String) { self.id = id; self.fw = fw; self.build = build; self.rolled_back = rolled_back; self.idf = idf }
}

/// whether a host is attached
public enum DongleUsbState: Equatable, Sendable, Codable {
    case up
    case down
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .up: return "up"
        case .down: return "down"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "up": self = .up
        case "down": self = .down
        default: self = .unknown(rawValue)
        }
    }
    public init(from decoder: Decoder) throws {
        self.init(rawValue: try decoder.singleValueContainer().decode(String.self))
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
    public static let all: [DongleUsbState] = [.up, .down]
}

/// The wire to the phone.
public struct DongleUsb: Codable, Equatable, Sendable {
    /// whether a host is attached
    public var state: DongleUsbState
    public init(state: DongleUsbState) { self.state = state }
}

/// idle: no network; searching: not seen on the air; joining: seen, getting an address; connected; failed: the attempt budget ran out
public enum DongleWifiState: Equatable, Sendable, Codable {
    case idle
    case searching
    case joining
    case connected
    case failed
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .idle: return "idle"
        case .searching: return "searching"
        case .joining: return "joining"
        case .connected: return "connected"
        case .failed: return "failed"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "idle": self = .idle
        case "searching": self = .searching
        case "joining": self = .joining
        case "connected": self = .connected
        case "failed": self = .failed
        default: self = .unknown(rawValue)
        }
    }
    public init(from decoder: Decoder) throws {
        self.init(rawValue: try decoder.singleValueContainer().decode(String.self))
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
    public static let all: [DongleWifiState] = [.idle, .searching, .joining, .connected, .failed]
}

/// the join budget
public struct DongleWifiAttempts: Codable, Equatable, Sendable {
    /// attempts spent on the current network
    public var used: Int
    /// the budget
    public var max: Int
    public init(used: Int, max: Int) { self.used = used; self.max = max }
}

/// The station: which network it was told, and how the join is going.
public struct DongleWifi: Codable, Equatable, Sendable {
    /// the network held since boot; empty when none was sent
    public var ssid: String
    /// a network has been sent since boot
    public var configured: Bool
    /// idle: no network; searching: not seen on the air; joining: seen, getting an address; connected; failed: the attempt budget ran out
    public var state: DongleWifiState
    /// the car's signal at the dongle, null unless connected
    public var rssi_dbm: Int?
    /// Wi-Fi channel, null unless connected
    public var channel: Int?
    /// the join budget
    public var attempts: DongleWifiAttempts
    public init(ssid: String, configured: Bool, state: DongleWifiState, rssi_dbm: Int?, channel: Int?, attempts: DongleWifiAttempts) { self.ssid = ssid; self.configured = configured; self.state = state; self.rssi_dbm = rssi_dbm; self.channel = channel; self.attempts = attempts }
}

/// the most recent forwarding failure, null when none since boot
public struct DongleRelayError: Codable, Equatable, Sendable {
    /// the POSIX errno
    public var errno: Int
    /// strerror of it
    public var message: String
    /// how many times in a row this errno repeated
    public var count: Int
    /// seconds since it last happened
    public var age_s: Int
    public init(errno: Int, message: String, count: Int, age_s: Int) { self.errno = errno; self.message = message; self.count = count; self.age_s = age_s }
}

/// What is being forwarded, and the last time forwarding failed.
public struct DongleRelay: Codable, Equatable, Sendable {
    /// control datagrams per second toward the car
    public var to_car_hz: Double
    /// datagrams per second toward the phone
    public var to_phone_hz: Double
    /// real-time sessions in use, of 4
    public var udp_sessions: Int
    /// HTTP connections in use, of 4
    public var tcp_connections: Int
    /// the most recent forwarding failure, null when none since boot
    public var last_error: DongleRelayError?
    public init(to_car_hz: Double, to_phone_hz: Double, udp_sessions: Int, tcp_connections: Int, last_error: DongleRelayError?) { self.to_car_hz = to_car_hz; self.to_phone_hz = to_phone_hz; self.udp_sessions = udp_sessions; self.tcp_connections = tcp_connections; self.last_error = last_error }
}

/// Uptime and memory.
public struct DongleSystem: Codable, Equatable, Sendable {
    /// seconds since boot
    public var uptime_s: Int
    /// free heap in bytes
    public var free_heap: Int
    public init(uptime_s: Int, free_heap: Int) { self.uptime_s = uptime_s; self.free_heap = free_heap }
}

/// GET /status: proto, then these groups.
public struct DongleStatus: Codable, Equatable, Sendable {
    /// the protocol version the device speaks
    public var proto: Int
    /// Who is answering.
    public var device: DongleDevice
    /// The wire to the phone.
    public var usb: DongleUsb
    /// The station: which network it was told, and how the join is going.
    public var wifi: DongleWifi
    /// What is being forwarded, and the last time forwarding failed.
    public var relay: DongleRelay
    /// Uptime and memory.
    public var system: DongleSystem
    public init(proto: Int, device: DongleDevice, usb: DongleUsb, wifi: DongleWifi, relay: DongleRelay, system: DongleSystem) { self.proto = proto; self.device = device; self.usb = usb; self.wifi = wifi; self.relay = relay; self.system = system }
}

/// POST /wifi answers with proto and these two fields of the wifi group, as now held.
public struct DongleWifiReply: Codable, Equatable, Sendable {
    /// the protocol version the device speaks
    public var proto: Int
    /// the network held since boot; empty when none was sent
    public var ssid: String
    /// idle: no network; searching: not seen on the air; joining: seen, getting an address; connected; failed: the attempt budget ran out
    public var state: DongleWifiState
    public init(proto: Int, ssid: String, state: DongleWifiState) { self.proto = proto; self.ssid = ssid; self.state = state }
}

/// The code inside an error envelope.
public enum DongleErrorCode: Equatable, Sendable, Codable {
    case bad_json
    case missing_field
    case unknown_field
    case wrong_type
    case bad_length
    case bad_chars
    case radio_refused
    case too_small
    case not_firmware
    case write_failed
    case busy
    case `internal`
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .bad_json: return "bad_json"
        case .missing_field: return "missing_field"
        case .unknown_field: return "unknown_field"
        case .wrong_type: return "wrong_type"
        case .bad_length: return "bad_length"
        case .bad_chars: return "bad_chars"
        case .radio_refused: return "radio_refused"
        case .too_small: return "too_small"
        case .not_firmware: return "not_firmware"
        case .write_failed: return "write_failed"
        case .busy: return "busy"
        case .`internal`: return "internal"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "bad_json": self = .bad_json
        case "missing_field": self = .missing_field
        case "unknown_field": self = .unknown_field
        case "wrong_type": self = .wrong_type
        case "bad_length": self = .bad_length
        case "bad_chars": self = .bad_chars
        case "radio_refused": self = .radio_refused
        case "too_small": self = .too_small
        case "not_firmware": self = .not_firmware
        case "write_failed": self = .write_failed
        case "busy": self = .busy
        case "internal": self = .`internal`
        default: self = .unknown(rawValue)
        }
    }
    public init(from decoder: Decoder) throws {
        self.init(rawValue: try decoder.singleValueContainer().decode(String.self))
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
    public static let all: [DongleErrorCode] = [.bad_json, .missing_field, .unknown_field, .wrong_type, .bad_length, .bad_chars, .radio_refused, .too_small, .not_firmware, .write_failed, .busy, .`internal`]
}

/// The error envelope every endpoint answers with, on 400 / 409 / 500.
public struct DongleAPIError: Codable, Equatable, Sendable {
    public var proto: Int
    public var error: Body
    public struct Body: Codable, Equatable, Sendable {
        public var code: DongleErrorCode
        public var message: String
        public var field: String?
        public init(code: DongleErrorCode, message: String, field: String?) { self.code = code; self.message = message; self.field = field }
    }
    public init(proto: Int, error: Body) { self.proto = proto; self.error = error }
}
