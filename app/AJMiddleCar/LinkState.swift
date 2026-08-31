import Foundation
import Network

/// Where the phone's network stands. `CarPath` produces it from two `NWPathMonitor`s.
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
    /// A car answered, but it is not ours. Both cars are a softAP serving the same API at the
    /// same address: whichever one the dongle is joined to is the one that answers here.
    case foreign(device: String)
    /// A car answered our hello naming a protocol version this app does not speak. The car
    /// answers a mismatched hello on purpose so this state can exist rather than a silent radar.
    case protoMismatch(theirs: Int)

    /// What is still true after the session that discovered it has ended.
    ///
    /// An identity the car told us about itself — someone else's name, or a protocol this build
    /// cannot speak — is not a transient failure to retry behind a radar sweep: the transport
    /// reopens every second or two, and re-deciding it from scratch each time flickers the screen
    /// that names the problem back to the radar the user has no reason to watch. Only the retry
    /// button clears them.
    ///
    /// It is one function because it has two callers — the session ending and the app being
    /// stopped — and the version of this that lived twice as an `if case` got `.protoMismatch`
    /// added to one copy and not the other.
    var survivingSessionEnd: SessionState {
        switch self {
        case .foreign, .protoMismatch: return self
        case .none, .adopted: return .none
        }
    }
}

/// The one liveness truth. Everything on screen — the status pill, the signal bars, the searching
/// overlay, the launch gate — reads this and nothing else.
enum Link: Equatable {
    case noDongle(NWPath.UnsatisfiedReason)
    case localNetworkDenied
    case searching
    case wrongCar(device: String)
    /// The car is there and talking, in a language from another build. Only a flash fixes it, so
    /// it is a screen that says so, not a retry loop.
    case wrongProto(theirs: Int)
    case live(Telemetry)

    var isLive: Bool { if case .live = self { return true }; return false }
    var telemetry: Telemetry? { if case .live(let t) = self { return t }; return nil }
}

/// The composition, kept pure so the truth table is host-tested rather than reasoned about.
enum LinkRule {
    /// Five missed pushes at the contract's telemetry rate. Telemetry is the only continuous
    /// evidence the car is still there, so its age is what "live" means.
    static let staleAfter: TimeInterval = 5 / Double(CarContract.telemetryHz)

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
        // Identity beats liveness: a car that answers with someone else's name — or in another
        // protocol version — must not be driven, however fresh its telemetry is.
        if case .foreign(let device) = session { return .wrongCar(device: device) }
        if case .protoMismatch(let theirs) = session { return .wrongProto(theirs: theirs) }
        guard case .adopted = session else { return .searching }
        guard let telemetry, let age, age < staleAfter else { return .searching }
        return .live(telemetry)
    }
}
