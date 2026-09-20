import Foundation

/// Whether the drive screen has a picture, and the moment it appeared. Pure, host-tested
/// (`app/tests/picturepresence`); `VideoLink` keeps one on its receive queue.
///
/// The «Нет картинки» placard hangs on `VideoLink.hasPicture`. That flag used to be set only
/// by the one-second stats tick, while the first frame went to the display layer the moment
/// it was reassembled — so the picture ran under the placard for up to a second (AJM-175).
/// The rule: a frame makes the picture present at once, and `frame(at:)` reports only the
/// transition from absent to present — the receiver hops to the main actor once per
/// appearance, not once per frame. The reverse transition, `staleAfter` without a frame,
/// stays with the tick, which asks `present(at:)`. `reset()` is a fresh socket: it owes its
/// own first frame, whatever the previous one delivered.
struct PicturePresence: Equatable {
    /// Without a frame for this long the picture is gone, as it always was.
    static let staleAfter: TimeInterval = 2

    private(set) var lastFrameAt: Date = .distantPast

    /// A frame was reassembled at `t`. True only when the picture *appeared* — there was none
    /// by `present(at: t)` just before this frame.
    mutating func frame(at t: Date) -> Bool {
        let appeared = !present(at: t)
        lastFrameAt = t
        return appeared
    }

    /// A frame landed less than `staleAfter` ago.
    func present(at t: Date) -> Bool {
        t.timeIntervalSince(lastFrameAt) < Self.staleAfter
    }

    mutating func reset() {
        lastFrameAt = .distantPast
    }
}
