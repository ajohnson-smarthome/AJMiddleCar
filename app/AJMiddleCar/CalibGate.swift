import Foundation

/// Whether the drive screen must hold the mandatory calibration wizard over itself. Pure,
/// host-tested (`app/tests/calibgate`): the car's `motors.calibrated` flag and the clock in,
/// the verdict out — asked on every telemetry frame, the first one included, so a car that is
/// uncalibrated when the session opens is caught the same as one that became uncalibrated on
/// an open screen. It replaced an `onChange` on the flag, which only ever saw a *change*: the
/// drive screen appears with the first frame already in hand, that frame's `false` was no
/// change, and the wizard never opened (AJM-63).
///
/// The one thing it remembers is when the car was last known calibrated — a `true` frame, or
/// the wizard closing on a save the car accepted. The frame the car computed before the write
/// still says `false`, and judged alone it would reopen the sheet mid-dismiss and flicker; a
/// `false` within `grace` of that moment is that stale frame, not a verdict. Value type, so the
/// memory lives exactly as long as the drive screen that holds it: a new session starts with
/// none and is judged by its own first frame, never by what the previous session saw.
struct CalibGate: Equatable {
    /// Seconds after the car was last known calibrated during which a `false` frame is stale.
    static let grace: TimeInterval = 2

    /// The gallery renders the drive screen statically: no session, no wizard.
    let preview: Bool
    private(set) var calibratedAt: TimeInterval?

    init(preview: Bool = false) { self.preview = preview }

    /// One frame's verdict. `nil` is no frame at all — nothing to judge, not an uncalibrated car.
    mutating func frame(calibrated: Bool?, now: TimeInterval) -> Bool {
        guard let calibrated, !preview else { return false }
        if calibrated { calibratedAt = now; return false }
        if let at = calibratedAt, now - at <= Self.grace { return false }
        return true
    }

    /// The wizard closed on a save the car accepted: known calibrated from now on, whatever the
    /// frame already in flight says.
    mutating func saved(now: TimeInterval) { calibratedAt = now }
}
