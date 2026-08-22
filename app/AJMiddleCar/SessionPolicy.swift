import Foundation

/// The session's pure decisions, extracted from `CarTransport` so they are host-tested.
/// The transport keeps the sockets and the clock; this answers "what does this reply mean"
/// and "how long do we wait" — the rules the cutover plan specifies and nothing pinned.
enum SessionPolicy {
    /// What one inbound datagram means to a handshake waiting on `sid`. A reply for another
    /// session id is a leftover from a previous socket; ignoring it is what makes ownership
    /// non-resumable rather than accidentally inherited.
    enum HandshakeOutcome: Equatable {
        case identity(device: String, fw: String)
        case protoMismatch(theirs: Int)
        case ignore
    }

    static func handshakeOutcome(_ inbound: RTFrame.Inbound?, sid: String) -> HandshakeOutcome {
        switch inbound {
        case .helloReply(let replySid, let device, let fw) where replySid == sid:
            return .identity(device: device, fw: fw)
        case .protoMismatch(let replySid, let theirs) where replySid == sid:
            return .protoMismatch(theirs: theirs)
        default:
            return .ignore
        }
    }

    /// Seconds before the next connection attempt, pre-jitter. Exponential, capped — and
    /// capped lower while no car has ever answered and the path is fine, because the
    /// contract asks for hello "repeated at ~5 Hz until answered", and a five-second sleep
    /// earned by a car that was not switched on yet is a car found five seconds late.
    static func backoffBase(attempt: Int,
                            pathBlocked: Bool,
                            everAdopted: Bool,
                            base: Double = 0.1,
                            cap: Double = 5.0,
                            discoveryCap: Double = 1.0) -> Double {
        let ceiling = (everAdopted || pathBlocked) ? cap : discoveryCap
        return min(ceiling, base * pow(2, Double(max(0, attempt - 1))))
    }

    /// How long a session holds after the car identified itself as undriveable (wrong car,
    /// wrong protocol) — long enough that the screen naming the problem is not a flicker
    /// between radar sweeps.
    static let identityHoldSeconds: Double = 10
}
