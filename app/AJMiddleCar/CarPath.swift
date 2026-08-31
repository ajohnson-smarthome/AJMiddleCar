import Foundation
import Network

/// Interface and permission truth, from two `NWPathMonitor`s and a clock.
///
/// Two monitors, because they answer different questions. Neither is asked whether the dongle is
/// present: that is `CarInterface.attached`, an address on the dongle's subnet, because a path
/// monitor restricted to the dongle answered "no" on hardware while the dongle was attached,
/// addressed and serving — the bug this class had. What the monitors carry is local-network
/// denial, which only they can report, and the Wi-Fi verdict the bench escape hatch needs.
///
/// And a clock, because fixing the *verdict* left the *trigger* wrong, which cost a second bench
/// session on 2026-09-01. `recompute()` used to run only from the two `pathUpdateHandler`s, so the
/// honest answer was only ever as fresh as iOS chose to make it — and iOS is under no obligation
/// here. The dongle advertises neither gateway nor DNS on purpose, so the system does not consider
/// its wire useful for general traffic; the update announcing it arrives late, or at cable-insert
/// time *before* DHCP has finished, when there is genuinely no `192.168.7.x` address to find yet.
/// Either way nothing asked again. An app launched with nothing plugged in was therefore born
/// `.noDongle` and stayed that way across the dongle actually arriving: the launch gate found it
/// (that half polls `/status` over a real socket, not a monitor), handed over, and the drive
/// screen was painted over with "no adapter" for as long as the monitors stayed quiet.
///
/// So presence re-answers on its own cadence now. The monitors remain — they still carry denial,
/// still supply the Wi-Fi verdict, and are still the fastest wake-up when they do fire.
///
/// Nothing here used to be read at all. Wi-Fi off, local network denied, wrong network and a
/// powered-off car were one indistinguishable radar.
@MainActor
final class CarPath: ObservableObject {
    @Published private(set) var state: PathState = .noDongle(.notAvailable)

    /// Consulted only under the bench escape hatch (`CarHost.direct`), where the car is addressed
    /// over the phone's own Wi-Fi and the interface type is known rather than assumed. In the
    /// ordinary dongle path, presence is not a monitor's verdict at all — see `recompute`.
    private let wifi = NWPathMonitor(requiredInterfaceType: .wifi)
    private let general = NWPathMonitor()
    private let queue = DispatchQueue(label: "car.path")
    private var wifiPath: NWPath?
    private var generalPath: NWPath?
    private var ticker: Task<Void, Never>?

    /// How often presence is re-asked without being prompted. A judgement, not a measurement:
    /// one `getifaddrs` is a syscall over a handful of interfaces, which is nothing beside the
    /// 10 Hz control frame this app already sends, and a second is short enough that no wrong
    /// screen survives long enough to read. It is a safety net rather than the main path —
    /// `refresh()` covers the one transition where even a second would show.
    private static let recheckInterval: Duration = .seconds(1)

    init() {
        wifi.pathUpdateHandler = { [weak self] p in
            Task { @MainActor in self?.wifiPath = p; self?.recompute() }
        }
        general.pathUpdateHandler = { [weak self] p in
            Task { @MainActor in self?.generalPath = p; self?.recompute() }
        }
        wifi.start(queue: queue)
        general.start(queue: queue)
        ticker = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: Self.recheckInterval)
                guard let self else { return }
                self.recompute()
            }
        }
    }

    deinit {
        ticker?.cancel()
        wifi.cancel()
        general.cancel()
    }

    /// Re-ask now. Called when the answer is about to be acted on and being a second stale would
    /// be visible — `CarLink.start()`, which is the moment something has just decided there is a
    /// car to talk to, and therefore the moment a leftover `.noDongle` becomes a screen.
    func refresh() { recompute() }

    private func recompute() {
        let next = verdict()
        // `@Published` emits on assignment whether or not the value changed, and this now runs
        // once a second forever — so an unchanged verdict must not re-invalidate every view
        // downstream of `CarLink.state`. `AppFlow.setPhase` guards itself for the same reason.
        guard state != next else { return }
        state = next
    }

    private func verdict() -> PathState {
        // Denial is checked first and on either monitor: it is the one state waiting cannot fix,
        // and it must never be rendered as "searching".
        for path in [generalPath, wifiPath] {
            if path?.status == .unsatisfied, path?.unsatisfiedReason == .localNetworkDenied {
                return .localNetworkDenied
            }
        }
        #if targetEnvironment(simulator)
        // The mock is reached over whatever the Mac uses — often Ethernet, sometimes loopback —
        // so requiring Wi-Fi here would strand every simulator session on a "no Wi-Fi" screen.
        return generalPath?.status == .satisfied
            ? .dongleUp : .noDongle(generalPath?.unsatisfiedReason ?? .notAvailable)
        #else
        if CarHost.direct {
            return wifiPath?.status == .satisfied
                ? .dongleUp : .noDongle(wifiPath?.unsatisfiedReason ?? .notAvailable)
        }
        // Presence is an address that either exists or does not, not a monitor's opinion of
        // whether the wire is worth offering. The dongle advertises neither gateway nor DNS on
        // purpose, so a restricted monitor can report an attached, working, fully reachable
        // dongle as unsatisfied — which is what stranded this screen on hardware. The same
        // indifference is why this is re-asked on a timer and not only when a path update
        // happens to arrive: see the type doc.
        return CarInterface.attached
            ? .dongleUp : .noDongle(generalPath?.unsatisfiedReason ?? .notAvailable)
        #endif
    }
}
