import Foundation

/// One board's step of the launch ladder — the same words for both boards, the copy chosen by
/// the device. Screen ids in the "AJMiddleCar screens" artifact are in parentheses.
public enum GateStep: Equatable {
    case seeking                 // asking, nothing answered yet (adapter S1; car S30 while it boots)
    case absent                  // nothing answers and a person is needed (adapter S2)
    case checking                // answered (a document or 404), being looked over — once per stage (S3 / S30)
    case fault                   // answered with something that is not the document (S7 / S33)
    case denied                  // iOS refused local-network access (S8)
    case wrongDevice(String)     // /version.device is not ours (S9 / S23) — carries what it called itself
    case rolledBack              // /version.rolled_back (S10 / S31)
    case updating                // behind the release, or 404: FirmwareView(forced) (S11 / S27)
    // Reaching a board through another one — the car through the adapter (S12 / S13 / S29 / S14).
    case sendingNetwork, searching, joining, joinFailed
}

/// What `Board.reach()` said about getting to the board this poll.
public enum Reach: Equatable {
    case reached            // ask it: `/version` may be read
    case hold(GateStep)     // not there yet — show this step, poll again
    case lost               // the board this one is reached through is gone: fall back a rung
}

/// One rung of the ladder: everything the shared stage needs about a board. The pure decision
/// (`StageRule`) reads only `Identity`; the closures are `AppFlow`'s side (HTTP, the relay).
public struct Board {
    public struct Identity: Equatable {
        public let device: UpdateRules.Device
        public let expectedDevice: String     // DongleContract.device / CarContract.device
        /// What silence on `/version` means here: the adapter needs a person (.absent), the car
        /// behind a joined adapter is booting (.seeking).
        public let silentStep: GateStep
        public init(device: UpdateRules.Device, expectedDevice: String, silentStep: GateStep) {
            self.device = device; self.expectedDevice = expectedDevice; self.silentStep = silentStep
        }
    }
    public let identity: Board.Identity
    /// The rung this board is reached through — `0` for the car behind the adapter, `nil` for a
    /// board reached directly. `reach` returning `.lost` sends the runner here.
    public let reachedThrough: Int?
    public let readVersion: () async throws -> Data
    public let reach: () async -> Reach
    public init(identity: Board.Identity, reachedThrough: Int?,
                readVersion: @escaping () async throws -> Data, reach: @escaping () async -> Reach) {
        self.identity = identity; self.reachedThrough = reachedThrough
        self.readVersion = readVersion; self.reach = reach
    }
}

/// The decision of one poll of one board's stage, pure over what the poll saw.
public enum StageRule {
    public enum Verdict: Equatable {
        case show(GateStep)   // set the phase to this step, pause, poll again
        case needRelease      // the board answered and the tag is unknown: fetch it, then poll again
        case ok               // this board is current and ours: advance to the next rung
        case lost             // the board this one is reached through is gone: fall back a rung
    }

    /// `version` is nil exactly when `reach` was not `.reached` (nothing was asked). When
    /// `reach == .reached`, `version` must be non-nil. `flashed` is what this phone last
    /// flashed into this board (`UpdateClient.lastFlashedTag`) — `VersionRule`'s reference for
    /// a rolled-back board.
    public static func decide(reach: Reach, version: VersionReply?, board: Board.Identity,
                              latestTag: String?, flashed: String?, rollback: RollbackChoice) -> Verdict {
        switch reach {
        case .lost: return .lost
        case .hold(let step): return .show(step)
        case .reached: break
        }
        guard let version else { return .show(board.silentStep) }  // defensive: reached ⇒ version
        // A board that answered — a document or a 404 — needs the release established before the
        // rule can compare it. Silence is not "answered": it goes straight to the silent step.
        if answered(version), latestTag == nil { return .needRelease }
        switch VersionRule.step(reply: version, expectedDevice: board.expectedDevice,
                                latestTag: latestTag ?? "", flashed: flashed, rollback: rollback) {
        case .plugIn: return .show(board.silentStep)
        case .faulty: return .show(.fault)
        case .accessDenied: return .show(.denied)
        case .wrongDevice(let name): return .show(.wrongDevice(name))
        case .rolledBack: return .show(.rolledBack)
        case .updating: return .show(.updating)
        case .ok: return .ok
        }
    }

    /// A read that carried a board's own bytes — a `/version` document or a 404. `.silent`,
    /// `.faulty` and `.denied` did not.
    private static func answered(_ reply: VersionReply) -> Bool {
        switch reply { case .version, .absent: return true; case .silent, .faulty, .denied: return false }
    }
}
