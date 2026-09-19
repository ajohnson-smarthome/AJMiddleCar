import Foundation

/// The third post-gate guard (`app/launch-ladder` → «Стражи после гейта»): a search that has
/// outlasted the adapter's join budget is a question for the adapter, not for the car.
///
/// After the gate the only thing asking the car anything is the session's `hello`, retried for
/// ever with a 5 s ceiling. The adapter, meanwhile, is bounded on purpose (`dongle/car-join`):
/// five join attempts, then `failed`, held until the next `POST /wifi` — and only the car's stage
/// of the ladder ever sent one. So a car that rebooted, or drove out of range for longer than the
/// budget, left the phone on «Здороваюсь с машинкой» for good, with the adapter's own panel
/// saying «Попытки исчерпаны» to nobody (AJM-95). The same hole under the forced-update screen,
/// whose wait for the car's `/version` is the same wait with the ladder parked (AJM-125,
/// AJM-134).
///
/// Pure: the caller keeps the clock and reads `/status`; this says when to read and what the
/// reading means. The act differs by caller. After the gate it is `AppFlow.restart(from: .car)`,
/// so the car's stage hands the network over again and shows «не удалось подключиться /
/// Повторить» if that does not help either. Under the forced update it is a `POST /wifi` and
/// nothing else, since parking the ladder there is deliberate — `updateFinished` is what
/// unparks it, and the screen's own wait for the car goes on.
public enum SearchGuard {
    /// How long a search may run before the adapter is asked. The adapter's budget is five
    /// attempts, ~10 s on the bench; 15 s keeps a margin over it, so an adapter still
    /// legitimately trying is never second-guessed — only one that has already given up. The
    /// same clock paces the re-asks: one `/status` per threshold, not one per poll.
    public static let threshold: TimeInterval = 15

    /// Whether `elapsed` seconds of searching is long enough to read the adapter's `/status`.
    public static func due(searchingFor elapsed: TimeInterval) -> Bool { elapsed >= threshold }

    public enum Verdict: Equatable {
        /// The adapter is still working (`searching`, `joining`), or already there
        /// (`connected` — the car is booting behind it, or the hello is on its way), or did not
        /// answer usably — keep waiting. The adapter's own silence is the wire guards' business.
        case wait
        /// The adapter will not get any further on its own: `failed` or `idle` with the car's
        /// network, or the car's network not handed to it at all. Hand the network over again —
        /// `retry` when it has the network and gave up, `configure` when it lacks it.
        case handNetwork(CarReach.Ask)
    }

    /// The same reading of `/status` the car's stage makes (`DongleLink.next`), narrowed to the
    /// one question the guard asks: does the adapter need the network again? `.silent`, `.faulty`
    /// and `.denied` are `.wait`: a transient `/status` blip must not restart anything, and an
    /// adapter that is really gone fires the no-adapter guard through the path monitor.
    public static func verdict(_ reply: DongleStatusReply, expectedSSID: String) -> Verdict {
        guard case .status(let s) = reply else { return .wait }
        switch DongleLink.next(status: s, expectedSSID: expectedSSID) {
        case .sendCredentials: return .handNetwork(.configure)
        case .retryJoin: return .handNetwork(.retry)
        case .searchingCar, .waiting, .readyForCar: return .wait
        }
    }
}
