import Foundation

/// What one read of the adapter's `/status` produced. Public, because `CarReach` classifies it
/// and the host test builds it. Read only after `VersionRule` said `.ok` (its shape depends on
/// the protocol `/version` just vouched for); a 404 here is a fault, not an older board.
public enum DongleStatusReply: Equatable {
    case status(DongleStatus)
    case silent    // nothing answered: no cable, refused connection, a deadline with no bytes
    case faulty    // answered and unusable: an HTTP error, a truncated stream, an undecodable body
    case denied    // iOS refused to let the request leave the phone

    /// Read a `/status` body as a document, else a fault.
    public static func decode(_ data: Data) -> DongleStatusReply {
        if let s = try? JSONDecoder().decode(DongleStatus.self, from: data) { return .status(s) }
        return .faulty
    }
    /// Classify what `DongleClient.statusData()` threw — `VersionReply.of`'s rule, with 404
    /// folded into `.faulty` (a `/status` fault, never an older board).
    public static func of(_ error: Error) -> DongleStatusReply {
        switch VersionReply.of(error) {
        case .version, .absent, .faulty: return .faulty
        case .silent: return .silent
        case .denied: return .denied
        }
    }
}

/// Reaching the car through the adapter: the adapter's network state machine with the join
/// budget, as one pure structure. `next` says what to show and what POST to send; the caller
/// (`AppFlow`) sends it. Identity, rollback and version are `VersionRule`'s, decided from the
/// car's own `/version` once this returns `.reached`.
public struct CarReach {
    public enum Ask: Equatable { case configure, retry }
    /// ONE, not three: each ask spends a full five-attempt budget on the adapter's side. The app
    /// asks once, the adapter tries five times, then both say so and wait for a person. See the
    /// removed `AppFlow.maxDongleJoinAttempts` doc for the whole history.
    public static let maxJoinAttempts = 1
    public private(set) var attempts = 0
    public private(set) var gaveUp = false
    public init() {}

    public mutating func next(_ reply: DongleStatusReply, expectedSSID: String) -> (reach: Reach, ask: Ask?) {
        guard case .status(let s) = reply else {
            // Silence / a bad answer / a denial from the ADAPTER is the adapter stage's verdict
            // (S2 / S7 / S8), not the car's — hand back a rung.
            return (.lost, nil)
        }
        switch DongleLink.next(status: s, expectedSSID: expectedSSID) {
        case .sendCredentials: return charge(.configure, showing: .sendingNetwork)
        case .searchingCar: return (.hold(.searching), nil)
        case .waiting: return (.hold(.joining), nil)
        case .retryJoin: return charge(.retry, showing: .searching)
        case .readyForCar:
            attempts = 0; gaveUp = false
            return (.reached, nil)
        }
    }

    /// «Повторить» on the join-failed screen: a fresh budget, spent from the next ask.
    public mutating func retry() { attempts = 0; gaveUp = false }

    /// Spend one budget for an ask, or hold at join-failed once it is gone. Mirrors the old
    /// `askDongleToJoin`: `guard !gaveUp`, then count this attempt.
    private mutating func charge(_ ask: Ask, showing step: GateStep) -> (reach: Reach, ask: Ask?) {
        guard !gaveUp else { return (.hold(.joinFailed), nil) }
        attempts += 1
        if attempts >= Self.maxJoinAttempts { gaveUp = true }
        return (.hold(step), ask)
    }
}
