// generated from contract/car-api.json by tools/gen_contract.py - do not edit

import Foundation

public enum CarContract {
    public static let proto = 1
    public static let device = "ajmiddlecar"
    public static let ssid = "AJMiddleCar"
    public static let password = "drive1234"
    public static let host = "192.168.4.1"
    public static let rtPort: UInt16 = 4210
    public static let maxDatagram = 320
    public static let maxCommand = 96
    public static let protoField = "proto"
    public static let deviceField = "device"
    public static let fwField = "fw"
    public static let throttleField = "t"
    public static let yawField = "y"
    public static let commandHz = 10
    public static let telemetryHz = 5
    public static let watchdogMs = 300
    public static let sessionIdleMs = 10000
    public static let helloField = "hello"
    public static let seqField = "seq"
    public static let byeField = "bye"
}

/// The fields the car sends in every telemetry datagram.
public enum TelemetryKey {
    /// monotonic frame counter from the car
    public static let seq = "seq"
    /// control frames the car received per second
    public static let rxFps = "rx_fps"
    /// AP-side signal for the station, 0 when unavailable
    public static let rssi = "rssi"
    /// control-watchdog trips since boot
    public static let wdtTrips = "wdt_trips"
    /// seconds since boot
    public static let uptimeS = "uptime_s"
    /// free heap in bytes
    public static let heap = "heap"
    /// a valid calibration is loaded
    public static let calibrated = "calibrated"
    /// the motor driver is answering
    public static let busOk = "bus_ok"
    /// which source owns the actuator
    public static let ctl = "ctl"
}

/// The values the car reports in telemetry's `ctl` field.
public enum CtlOwner {
    public static let none = "none"
    public static let recover = "recover"
    public static let console = "console"
    public static let rt = "rt"
    public static let calib = "calib"
    public static let ota = "ota"
    public static let safe = "safe"
    public static let all = ["none", "recover", "console", "rt", "calib", "ota", "safe"]
}

/// Slew-rate limit on acceleration. Rise is bounded, fall is instant, so stopping is never delayed.
public struct Ramp: Codable, Equatable, Sendable {
    /// time from zero to full scale in ms; 0 disables the ramp
    public var ramp_ms: Int
    public init(ramp_ms: Int) { self.ramp_ms = ramp_ms }
}

public extension Ramp {
    static let path = "/ramp"
    static let `default` = Ramp(ramp_ms: 300)
    static let ramp_msRange: ClosedRange<Int> = 0...2000
}

/// Straight-line correction. Positive slows the left side, negative slows the right; it only ever attenuates.
public struct Trim: Codable, Equatable, Sendable {
    /// percentage by which the faster side is slowed
    public var trim_pct: Int
    public init(trim_pct: Int) { self.trim_pct = trim_pct }
}

public extension Trim {
    static let path = "/trim"
    static let `default` = Trim(trim_pct: 0)
    static let trim_pctRange: ClosedRange<Int> = -30...30
}

/// Reverse-replay retreat on unexpected link loss. A deliberate goodbye suppresses it.
public struct Recover: Codable, Equatable, Sendable {
    /// retrace on unexpected silence; when false the car stops instead
    public var enabled: Bool
    /// how far back the breadcrumb history reaches
    public var window_ms: Int
    public init(enabled: Bool, window_ms: Int) { self.enabled = enabled; self.window_ms = window_ms }
}

public extension Recover {
    static let path = "/recover"
    static let `default` = Recover(enabled: true, window_ms: 5000)
    static let window_msRange: ClosedRange<Int> = 1000...10000
}

/// Wheel and encoder geometry. Stored on the car; speed is not yet computed from it.
public struct Wheel: Codable, Equatable, Sendable {
    /// wheel diameter in mm
    public var diameter_mm: Int
    /// encoder pulses per motor-shaft revolution, one channel
    public var ppr: Int
    /// gear ratio times 100; 1:9 is 900
    public var gear_x100: Int
    /// quadrature edge multiplier
    public var quad: Int
    public init(diameter_mm: Int, ppr: Int, gear_x100: Int, quad: Int) { self.diameter_mm = diameter_mm; self.ppr = ppr; self.gear_x100 = gear_x100; self.quad = quad }
}

public extension Wheel {
    static let path = "/wheel"
    static let `default` = Wheel(diameter_mm: 65, ppr: 11, gear_x100: 900, quad: 4)
    static let diameter_mmRange: ClosedRange<Int> = 20...150
    static let pprRange: ClosedRange<Int> = 1...1000
    static let gear_x100Range: ClosedRange<Int> = 100...30000
    static let quadAllowed: [Int] = [1, 2, 4]
}

/// Distances between wheel centres. The track feeds the app's manoeuvre geometry.
public struct Dims: Codable, Equatable, Sendable {
    /// lateral distance between left and right wheel centres
    public var track_mm: Int
    /// longitudinal distance between front and rear wheel centres
    public var wheelbase_mm: Int
    public init(track_mm: Int, wheelbase_mm: Int) { self.track_mm = track_mm; self.wheelbase_mm = wheelbase_mm }
}

public extension Dims {
    static let path = "/dims"
    static let `default` = Dims(track_mm: 130, wheelbase_mm: 210)
    static let track_mmRange: ClosedRange<Int> = 60...300
    static let wheelbase_mmRange: ClosedRange<Int> = 90...360
}
