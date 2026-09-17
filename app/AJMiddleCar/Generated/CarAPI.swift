// generated from contract/car-api.json by tools/gen_contract.py - do not edit

import Foundation

public enum CarContract {
    public static let proto = 2
    public static let device = "ajmiddlecar"
    public static let ssid = "AJMiddleCar"
    public static let password = "drive1234"
    public static let rtPort: UInt16 = 4210
    public static let maxDatagram = 320
    public static let maxCommand = 96
    public static let commandHz = 10
    public static let telemetryHz = 5
    public static let watchdogMs = 300
    public static let sessionIdleMs = 10000
    public static let videoPort: UInt16 = 4211
    public static let videoWidth = 1280
    public static let videoHeight = 720
    public static let videoFps = 22
    public static let videoChunkBytes = 1400
    public static let videoHeaderBytes = 12
    public static let videoWireProto = 1
    public static let videoSubscribeMs = 1000
    public static let videoSubscribeTimeoutMs = 3000
    public static let videoKeyframeS = 10
    public static let videoIdrMinMs = 250
    public static let videoRotation = 0
    public static let videoMirror = false
    public static let protoField = "proto"
    public static let typeField = "type"
    public static let sessionField = "session"
    public static let seqField = "seq"
    public static let throttleField = "throttle"
    public static let turnField = "turn"
    public static let keyField = "key"
    public static let okField = "ok"
    public static let errorField = "error"
    public static let rootPath = "/"
    public static let statusPath = "/status"
    public static let versionPath = "/version"
    public static let configPath = "/config"
    public static let calibrationPath = "/calibration"
    public static let spinPath = "/calibration/spin"
    public static let otaPath = "/ota"
    public static let snapshotPath = "/snapshot"
}

/// The `type` word on every real-time datagram.
public enum RTType {
    public static let hello = "hello"
    public static let helloAck = "hello_ack"
    public static let drive = "drive"
    public static let bye = "bye"
    public static let telemetry = "telemetry"
    public static let view = "view"
}

/// Who is answering — the object in the hello reply. Version and identity for the launch gate come from /version.
public struct DeviceInfo: Codable, Equatable, Sendable {
    /// the device name; ajmiddlecar for this car
    public var id: String
    /// firmware version, v<semver>+<build>
    public var fw: String
    /// the number after + in fw, as an integer
    public var build: Int
    /// the bootloader reverted the last update
    public var rolled_back: Bool
    public init(id: String, fw: String, build: Int, rolled_back: Bool) { self.id = id; self.fw = fw; self.build = build; self.rolled_back = rolled_back }
}

/// The control link as the car sees it.
public struct LinkInfo: Codable, Equatable, Sendable {
    /// drive datagrams received per second
    public var rx_hz: Int
    /// the driving station's signal at the car, null when not measured
    public var rssi_dbm: Int?
    /// control-watchdog trips since boot
    public var timeouts: Int
    public init(rx_hz: Int, rssi_dbm: Int?, timeouts: Int) { self.rx_hz = rx_hz; self.rssi_dbm = rssi_dbm; self.timeouts = timeouts }
}

/// both PWM boards up and the last write accepted
public enum MotorsBus: Equatable, Sendable, Codable {
    case ok
    case down
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .ok: return "ok"
        case .down: return "down"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "ok": self = .ok
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
    public static let all: [MotorsBus] = [.ok, .down]
}

/// which source owns the actuator
public enum MotorsOwner: Equatable, Sendable, Codable {
    case idle
    case recovering
    case console
    case remote
    case calibration
    case update
    case safe_stop
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .idle: return "idle"
        case .recovering: return "recovering"
        case .console: return "console"
        case .remote: return "remote"
        case .calibration: return "calibration"
        case .update: return "update"
        case .safe_stop: return "safe_stop"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "idle": self = .idle
        case "recovering": self = .recovering
        case "console": self = .console
        case "remote": self = .remote
        case "calibration": self = .calibration
        case "update": self = .update
        case "safe_stop": self = .safe_stop
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
    public static let all: [MotorsOwner] = [.idle, .recovering, .console, .remote, .calibration, .update, .safe_stop]
}

/// The actuator: its bus, its calibration, and who is commanding it.
public struct MotorsInfo: Codable, Equatable, Sendable {
    /// both PWM boards up and the last write accepted
    public var bus: MotorsBus
    /// a valid wheel calibration is loaded
    public var calibrated: Bool
    /// which source owns the actuator
    public var owner: MotorsOwner
    public init(bus: MotorsBus, calibrated: Bool, owner: MotorsOwner) { self.bus = bus; self.calibrated = calibrated; self.owner = owner }
}

/// ok when fw equals expected
public enum RadioState: Equatable, Sendable, Codable {
    case ok
    case mismatch
    case unavailable
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .ok: return "ok"
        case .mismatch: return "mismatch"
        case .unavailable: return "unavailable"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "ok": self = .ok
        case "mismatch": self = .mismatch
        case "unavailable": self = .unavailable
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
    public static let all: [RadioState] = [.ok, .mismatch, .unavailable]
}

/// The C6 radio co-processor's firmware against what this build expects.
public struct RadioInfo: Codable, Equatable, Sendable {
    /// the radio's version, null when it did not answer
    public var fw: String?
    /// the version this build was made for
    public var expected: String
    /// ok when fw equals expected
    public var state: RadioState
    public init(fw: String?, expected: String, state: RadioState) { self.fw = fw; self.expected = expected; self.state = state }
}

/// What happened to the settings at this boot.
public struct StorageInfo: Codable, Equatable, Sendable {
    /// the NVS format changed and every setting went back to its default
    public var reset_at_boot: Bool
    public init(reset_at_boot: Bool) { self.reset_at_boot = reset_at_boot }
}

/// Uptime and memory.
public struct SystemInfo: Codable, Equatable, Sendable {
    /// seconds since boot
    public var uptime_s: Int
    /// free heap in bytes
    public var free_heap: Int
    public init(uptime_s: Int, free_heap: Int) { self.uptime_s = uptime_s; self.free_heap = free_heap }
}

/// off: no sensor answered at boot; idle: sensor in standby, nobody watching; streaming: encoding for the driver
public enum VideoState: Equatable, Sendable, Codable {
    case off
    case idle
    case streaming
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .off: return "off"
        case .idle: return "idle"
        case .streaming: return "streaming"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "off": self = .off
        case "idle": self = .idle
        case "streaming": self = .streaming
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
    public static let all: [VideoState] = [.off, .idle, .streaming]
}

/// The camera and the FPV stream.
public struct VideoInfo: Codable, Equatable, Sendable {
    /// off: no sensor answered at boot; idle: sensor in standby, nobody watching; streaming: encoding for the driver
    public var state: VideoState
    /// frames encoded in the last second
    public var fps: Int
    /// kbit sent in the last second
    public var kbps: Int
    /// frames not sent since boot: the sender was still busy with the previous frame, the encoder overflowed, or the frame needed more than 255 chunks
    public var dropped: Int
    public init(state: VideoState, fps: Int, kbps: Int, dropped: Int) { self.state = state; self.fps = fps; self.kbps = kbps; self.dropped = dropped }
}

/// The 5 Hz push: proto, type, seq, then these groups.
public struct Telemetry: Codable, Equatable, Sendable {
    /// the protocol version the device speaks
    public var proto: Int
    /// the car's own push counter
    public var seq: Int
    /// The control link as the car sees it.
    public var link: LinkInfo
    /// The actuator: its bus, its calibration, and who is commanding it.
    public var motors: MotorsInfo
    /// Uptime and memory.
    public var system: SystemInfo
    /// The camera and the FPV stream.
    public var video: VideoInfo
    public init(proto: Int, seq: Int, link: LinkInfo, motors: MotorsInfo, system: SystemInfo, video: VideoInfo) { self.proto = proto; self.seq = seq; self.link = link; self.motors = motors; self.system = system; self.video = video }
}

/// GET /status: proto, then these groups.
public struct CarStatus: Codable, Equatable, Sendable {
    /// the protocol version the device speaks
    public var proto: Int
    /// The control link as the car sees it.
    public var link: LinkInfo
    /// The actuator: its bus, its calibration, and who is commanding it.
    public var motors: MotorsInfo
    /// The C6 radio co-processor's firmware against what this build expects.
    public var radio: RadioInfo
    /// What happened to the settings at this boot.
    public var storage: StorageInfo
    /// Uptime and memory.
    public var system: SystemInfo
    /// The camera and the FPV stream.
    public var video: VideoInfo
    public init(proto: Int, link: LinkInfo, motors: MotorsInfo, radio: RadioInfo, storage: StorageInfo, system: SystemInfo, video: VideoInfo) { self.proto = proto; self.link = link; self.motors = motors; self.radio = radio; self.storage = storage; self.system = system; self.video = video }
}

/// Slew-rate limit on acceleration. Rise is bounded, fall is instant, so stopping is never delayed.
public struct Ramp: Codable, Equatable, Sendable {
    /// time from zero to full scale in ms; 0 disables the ramp
    public var rise_ms: Int
    public init(rise_ms: Int) { self.rise_ms = rise_ms }
}

public extension Ramp {
    static let key = "ramp"
    static let `default` = Ramp(rise_ms: 300)
    static let rise_msRange: ClosedRange<Int> = 0...2000
    static func pick(from c: CarConfig) -> Ramp? { c.ramp }
    static func wrap(_ v: Ramp) -> CarConfig { CarConfig(ramp: v) }
}

/// Straight-line correction. Positive slows the left side, negative slows the right; it only ever attenuates.
public struct Trim: Codable, Equatable, Sendable {
    /// percentage by which the faster side is slowed
    public var balance_pct: Int
    public init(balance_pct: Int) { self.balance_pct = balance_pct }
}

public extension Trim {
    static let key = "trim"
    static let `default` = Trim(balance_pct: 0)
    static let balance_pctRange: ClosedRange<Int> = -30...30
    static func pick(from c: CarConfig) -> Trim? { c.trim }
    static func wrap(_ v: Trim) -> CarConfig { CarConfig(trim: v) }
}

/// Reverse-replay retreat on unexpected link loss. A deliberate goodbye suppresses it.
public struct Recovery: Codable, Equatable, Sendable {
    /// retrace on unexpected silence; when false the car stops instead
    public var enabled: Bool
    /// how far back the breadcrumb history reaches
    public var window_ms: Int
    public init(enabled: Bool, window_ms: Int) { self.enabled = enabled; self.window_ms = window_ms }
}

public extension Recovery {
    static let key = "recovery"
    static let `default` = Recovery(enabled: true, window_ms: 5000)
    static let window_msRange: ClosedRange<Int> = 1000...10000
    static func pick(from c: CarConfig) -> Recovery? { c.recovery }
    static func wrap(_ v: Recovery) -> CarConfig { CarConfig(recovery: v) }
}

/// Wheel and encoder geometry. Stored on the car; speed is not yet computed from it.
public struct Wheel: Codable, Equatable, Sendable {
    /// wheel diameter in mm
    public var diameter_mm: Int
    /// encoder pulses per motor-shaft revolution, one channel
    public var encoder_ppr: Int
    /// gear ratio as a decimal; 1:9 is 9.0 (held as ratio x100 inside)
    public var gear_ratio: Double
    /// quadrature edge multiplier
    public var quadrature: Int
    public init(diameter_mm: Int, encoder_ppr: Int, gear_ratio: Double, quadrature: Int) { self.diameter_mm = diameter_mm; self.encoder_ppr = encoder_ppr; self.gear_ratio = gear_ratio; self.quadrature = quadrature }
}

public extension Wheel {
    static let key = "wheel"
    static let `default` = Wheel(diameter_mm: 65, encoder_ppr: 11, gear_ratio: 9.0, quadrature: 4)
    static let diameter_mmRange: ClosedRange<Int> = 20...150
    static let encoder_pprRange: ClosedRange<Int> = 1...1000
    static let gear_ratioRange: ClosedRange<Double> = 1.0...300.0
    static let quadratureAllowed: [Int] = [1, 2, 4]
    static func pick(from c: CarConfig) -> Wheel? { c.wheel }
    static func wrap(_ v: Wheel) -> CarConfig { CarConfig(wheel: v) }
}

/// Distances between wheel centres. The track feeds the app's manoeuvre geometry.
public struct Chassis: Codable, Equatable, Sendable {
    /// lateral distance between left and right wheel centres
    public var track_mm: Int
    /// longitudinal distance between front and rear wheel centres
    public var wheelbase_mm: Int
    public init(track_mm: Int, wheelbase_mm: Int) { self.track_mm = track_mm; self.wheelbase_mm = wheelbase_mm }
}

public extension Chassis {
    static let key = "chassis"
    static let `default` = Chassis(track_mm: 130, wheelbase_mm: 210)
    static let track_mmRange: ClosedRange<Int> = 60...300
    static let wheelbase_mmRange: ClosedRange<Int> = 90...360
    static func pick(from c: CarConfig) -> Chassis? { c.chassis }
    static func wrap(_ v: Chassis) -> CarConfig { CarConfig(chassis: v) }
}

/// The FPV encoder and the video switch. bitrate_kbps applies at the next stream start; enabled applies at once.
public struct Video: Codable, Equatable, Sendable {
    /// target H.264 bitrate in kbit/s; the adapter's USB (Full-Speed, one transfer block per host read) drains ~4 Mbit/s at the car's 3 ms chunk pacing, measured 2026-09-16, and the car's own pacing caps it at 3.7 — 2500 leaves the gap for keyframes and motion
    public var bitrate_kbps: Int
    /// the car streams video at all; false and it ignores every view, ends a running subscription on the video control task's next tick (≤100 ms; the encoder and the sensor follow within a frame) and puts the sensor in standby — the drive screen's video button, remembered on the car
    public var enabled: Bool
    public init(bitrate_kbps: Int, enabled: Bool) { self.bitrate_kbps = bitrate_kbps; self.enabled = enabled }
}

public extension Video {
    static let key = "video"
    static let `default` = Video(bitrate_kbps: 2500, enabled: true)
    static let bitrate_kbpsRange: ClosedRange<Int> = 500...3000
    static func pick(from c: CarConfig) -> Video? { c.video }
    static func wrap(_ v: Video) -> CarConfig { CarConfig(video: v) }
}

/// GET returns every domain; POST takes any subset of domains, each complete, validates the whole body before applying any of it, and answers with the full configuration as now held.
public struct CarConfig: Codable, Equatable, Sendable {
    public var proto: Int?
    public var ramp: Ramp?
    public var trim: Trim?
    public var recovery: Recovery?
    public var wheel: Wheel?
    public var chassis: Chassis?
    public var video: Video?
    public init(proto: Int? = nil, ramp: Ramp? = nil, trim: Trim? = nil, recovery: Recovery? = nil, wheel: Wheel? = nil, chassis: Chassis? = nil, video: Video? = nil) { self.proto = proto; self.ramp = ramp; self.trim = trim; self.recovery = recovery; self.wheel = wheel; self.chassis = chassis; self.video = video }
}

/// A wheel's corner, by name.
public enum CalibCorner: Equatable, Sendable, Codable {
    case front_left
    case front_right
    case rear_left
    case rear_right
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .front_left: return "front_left"
        case .front_right: return "front_right"
        case .rear_left: return "rear_left"
        case .rear_right: return "rear_right"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "front_left": self = .front_left
        case "front_right": self = .front_right
        case "rear_left": self = .rear_left
        case "rear_right": self = .rear_right
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
    public static let all: [CalibCorner] = [.front_left, .front_right, .rear_left, .rear_right]
}

/// Which way to spin a pair.
public enum CalibDirection: Equatable, Sendable, Codable {
    case forward
    case reverse
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .forward: return "forward"
        case .reverse: return "reverse"
        case .unknown(let raw): return raw
        }
    }
    public init(rawValue: String) {
        switch rawValue {
        case "forward": self = .forward
        case "reverse": self = .reverse
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
    public static let all: [CalibDirection] = [.forward, .reverse]
}

/// One wheel of the calibration table.
public struct CalibWheel: Codable, Equatable, Sendable {
    /// which corner this row describes
    public var corner: CalibCorner
    /// the channel pair driving it, 0..3
    public var pair: Int
    /// true when the pair's A channel drives it backwards
    public var inverted: Bool
    public init(corner: CalibCorner, pair: Int, inverted: Bool) { self.corner = corner; self.pair = pair; self.inverted = inverted }
}

/// GET /calibration returns calibrated and the wheels table (empty when not calibrated). POST /calibration takes wheels: four corners, each once, pairs 0..3 each once, inverted per wheel. POST /calibration/spin takes pair and direction.
public struct Calibration: Codable, Equatable, Sendable {
    /// the protocol version the device speaks
    public var proto: Int
    /// a valid table is loaded
    public var calibrated: Bool
    /// the table, empty when not calibrated
    public var wheels: [CalibWheel]
    public init(proto: Int, calibrated: Bool, wheels: [CalibWheel]) { self.proto = proto; self.calibrated = calibrated; self.wheels = wheels }
}

/// The code inside an error envelope.
public enum CarErrorCode: Equatable, Sendable, Codable {
    case bad_json
    case missing_field
    case unknown_field
    case wrong_type
    case out_of_range
    case not_allowed
    case busy
    case too_small
    case not_firmware
    case write_failed
    case `internal`
    case unknown(String)
    public var rawValue: String {
        switch self {
        case .bad_json: return "bad_json"
        case .missing_field: return "missing_field"
        case .unknown_field: return "unknown_field"
        case .wrong_type: return "wrong_type"
        case .out_of_range: return "out_of_range"
        case .not_allowed: return "not_allowed"
        case .busy: return "busy"
        case .too_small: return "too_small"
        case .not_firmware: return "not_firmware"
        case .write_failed: return "write_failed"
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
        case "out_of_range": self = .out_of_range
        case "not_allowed": self = .not_allowed
        case "busy": self = .busy
        case "too_small": self = .too_small
        case "not_firmware": self = .not_firmware
        case "write_failed": self = .write_failed
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
    public static let all: [CarErrorCode] = [.bad_json, .missing_field, .unknown_field, .wrong_type, .out_of_range, .not_allowed, .busy, .too_small, .not_firmware, .write_failed, .`internal`]
}

/// The error envelope every endpoint answers with, on 400 / 409 / 500.
public struct CarAPIError: Codable, Equatable, Sendable {
    public var proto: Int
    public var error: Body
    public struct Body: Codable, Equatable, Sendable {
        public var code: CarErrorCode
        public var message: String
        public var field: String?
        public init(code: CarErrorCode, message: String, field: String?) { self.code = code; self.message = message; self.field = field }
    }
    public init(proto: Int, error: Body) { self.proto = proto; self.error = error }
}
