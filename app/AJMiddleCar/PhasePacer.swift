import Foundation

/// How long a startup screen is allowed to be on screen for — the rule that keeps a sequence of
/// six steps from becoming six flashes.
///
/// The startup ladder is only worth showing if it can be read. On healthy hardware several of its
/// steps resolve in tens of milliseconds: the adapter answers at once, the release check hits a
/// warm cache, the car is already joined. Rendered as they arrive, those steps do not inform
/// anybody — they strobe, which is worse than not showing them at all and is the complaint this
/// whole redesign started from.
///
/// So a screen that has been displayed stays displayed for `minVisible`, and the next one waits.
/// Nothing is skipped: every step of the ladder is seen, in order, which is the point of having
/// named them. The cost is bounded and known — a launch where *everything* is instant takes
/// `minVisible` per step, and no more, because a step that takes longer than that on its own is
/// never delayed at all.
///
/// Pure, so the cost is arithmetic anybody can check rather than a feeling about a stopwatch.
enum PhasePacer {
    /// Long enough to read three words and register that the picture changed; short enough that
    /// six of them are a launch rather than a wait.
    static let minVisible: TimeInterval = 0.40

    /// Seconds the caller must wait before replacing a screen first shown at `shownAt`.
    static func wait(shownAt: TimeInterval, now: TimeInterval,
                     minVisible: TimeInterval = Self.minVisible) -> TimeInterval {
        max(0, minVisible - (now - shownAt))
    }

    /// When each phase actually reaches the screen, given when each became true.
    ///
    /// This is the whole policy in one expression, and it exists so the trade-off can be argued
    /// about with numbers: feed it a launch's real timings and it says exactly how much the
    /// pacing costs. `arrivals` must be non-decreasing — they are timestamps of successive state
    /// changes, so they are by construction.
    static func schedule(arrivals: [TimeInterval],
                         minVisible: TimeInterval = Self.minVisible) -> [TimeInterval] {
        var out: [TimeInterval] = []
        out.reserveCapacity(arrivals.count)
        var previous: TimeInterval?
        for a in arrivals {
            let at = previous.map { max(a, $0 + minVisible) } ?? a
            out.append(at)
            previous = at
        }
        return out
    }
}
