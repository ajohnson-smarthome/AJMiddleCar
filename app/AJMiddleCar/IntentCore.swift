import Foundation

/// The pure core of the command authority: who is speaking, and whether a given trick step still
/// has the right to.
///
/// Kept apart from `ControlIntent` so the priority rule is host-tested rather than reasoned
/// about. The rule is one sentence: **manual input wins immediately, without awaiting anything.**
/// Cancelling the trick's task is not enough on its own — a step computed before the touch can
/// still be delivered after it — so every step carries the epoch it was started under and is
/// refused once that epoch is stale.
struct IntentCore: Equatable {
    enum Source: Equatable { case idle, manual, trick }

    private(set) var epoch: UInt64 = 0
    private(set) var t: Double = 0
    private(set) var y: Double = 0
    private(set) var source: Source = .idle

    /// Returns true when this input pre-empted a running trick, which is the caller's cue to
    /// tear the trick's task down. The command itself is already the driver's.
    @discardableResult
    mutating func manual(t: Double, y: Double) -> Bool {
        let preempted = source == .trick
        if preempted { epoch &+= 1 }
        source = .manual
        self.t = ControlModel.clamp(t)
        self.y = ControlModel.clamp(y)
        return preempted
    }

    /// Claim the channel for a trick. The returned epoch is that trick's licence to speak.
    mutating func beginTrick() -> UInt64 {
        epoch &+= 1
        source = .trick
        return epoch
    }

    /// One step of a trick. Refused — and silently ignored — once anything else has taken over.
    @discardableResult
    mutating func trickStep(epoch: UInt64, t: Double, y: Double) -> Bool {
        guard source == .trick, epoch == self.epoch else { return false }
        self.t = ControlModel.clamp(t)
        self.y = ControlModel.clamp(y)
        return true
    }

    /// End a trick. `stop` zeroes the command, which is right when the trick ran to its end or
    /// the user pressed stop — and wrong after a pre-emption, where the driver already owns the
    /// command. Refusing the stale epoch is what tells the two apart.
    @discardableResult
    mutating func endTrick(epoch: UInt64, stop: Bool) -> Bool {
        guard source == .trick, epoch == self.epoch else { return false }
        source = .idle
        if stop { t = 0; y = 0 }
        return true
    }
}
