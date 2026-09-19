import Foundation
import Network

/// Where the phone's network stands. `CarPath` produces it from an `NWPathMonitor` and the
/// dongle's address.
///
/// The dongle unplugged, local network denied, and the dongle plugged in but with no car
/// answering are three different problems with three different fixes, and the app used to
/// render all of them as one endless radar.
enum PathState: Equatable {
    case dongleUp
    case noDongle(NWPath.UnsatisfiedReason)
    /// The user denied local-network access. Nothing we send leaves the phone, and no amount of
    /// waiting changes that — its only signal is `unsatisfiedReason`, which nothing used to read.
    case localNetworkDenied
}

/// Where the session stands, as told by the car itself in its hello reply.
enum SessionState: Equatable {
    case none
    case adopted(device: String, fw: String)
}

/// The one liveness truth. Everything on screen — the status pill, the signal bars, the searching
/// overlay, the launch gate — reads this and nothing else.
enum Link: Equatable {
    case noDongle(NWPath.UnsatisfiedReason)
    case localNetworkDenied
    case searching
    case live(Telemetry)

    var isLive: Bool { if case .live = self { return true }; return false }
    /// Whether the interface itself is gone — asked as a transition (`was`, `is no longer`) by
    /// the root view, which turns it into "the wire came back" and re-enters the dongle gate.
    var isNoDongle: Bool { if case .noDongle = self { return true }; return false }
    var telemetry: Telemetry? { if case .live(let t) = self { return t }; return nil }
}

/// The composition, kept pure so the truth table is host-tested rather than reasoned about.
enum LinkRule {
    /// Five missed pushes at the contract's telemetry rate. Telemetry is the only continuous
    /// evidence the car is still there, so its age is what "live" means.
    /// How long telemetry may go quiet before the drive screen gives way to "saying hello".
    ///
    /// It was five frames — one second at the contract's 5 Hz — and that is too tight for what it
    /// costs. Telemetry is UDP, and now it crosses a Wi-Fi hop and a USB relay as well; a
    /// one-second gap is not rare, and each one replaced the whole drive screen with a connection
    /// screen and then took it back. The link was fine the entire time.
    ///
    /// Twelve frames instead. Nothing safety-critical rests on this number: the car's own control
    /// watchdog is 300 ms and lives in the firmware, where losing the driver actually matters.
    /// This one governs a screen, and a screen that flickers teaches its reader to distrust it.
    static let staleAfter: TimeInterval = 12 / Double(CarContract.telemetryHz)

    /// `.live` requires all three: the path is satisfied, the car adopted our session, and the
    /// newest telemetry is fresh. Any one of them missing is `.searching` — never `.live` with a
    /// caveat, because a caveat is what the old three-variable model was.
    static func compose(path: PathState,
                        session: SessionState,
                        telemetry: Telemetry?,
                        age: TimeInterval?) -> Link {
        switch path {
        case .localNetworkDenied: return .localNetworkDenied
        case .noDongle(let reason): return .noDongle(reason)
        case .dongleUp: break
        }
        guard case .adopted = session else { return .searching }
        guard let telemetry, let age, age < staleAfter else { return .searching }
        return .live(telemetry)
    }

    /// Whether the drive screen, once shown, still stands. `compose` folds "the session is
    /// open but telemetry is late" and "there is no session" into one `.searching`, which is
    /// right for the label on screen and wrong for the screen's existence: a telemetry pause
    /// shorter than the stall that ends the session used to swap the drive screen for the
    /// radar, and the sheets on it — the wizard's assignments, the settings stack — went with
    /// it (AJM-107). The screen is born on `.live` and lives exactly while this holds: the
    /// path up, the session adopted, and a frame seen in *this* session — `telemetry` is
    /// cleared with the session, so a fresh one is not born on the previous one's memory.
    /// Path and session outrank the frame here as they do in `compose`.
    static func inSession(path: PathState, session: SessionState, telemetry: Telemetry?) -> Bool {
        guard case .dongleUp = path, case .adopted = session else { return false }
        return telemetry != nil
    }
}
