import Foundation
import Network

/// Interface and permission truth, from two `NWPathMonitor`s.
///
/// Two, because they answer different questions. Neither is asked whether the dongle is present:
/// that is `CarInterface.attached`, an address on the dongle's subnet, because a path monitor
/// restricted to the dongle answered "no" on hardware while the dongle was attached, addressed
/// and serving — the bug this class had. What the monitors carry is local-network denial, which
/// only they can report, and the Wi-Fi verdict the bench escape hatch needs. They are still the
/// thing that *wakes* the recomputation: the interface set changing is a path change.
///
/// Nothing here used to be read. Wi-Fi off, local network denied, wrong network and a powered-off
/// car were one indistinguishable radar.
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

    init() {
        wifi.pathUpdateHandler = { [weak self] p in
            Task { @MainActor in self?.wifiPath = p; self?.recompute() }
        }
        general.pathUpdateHandler = { [weak self] p in
            Task { @MainActor in self?.generalPath = p; self?.recompute() }
        }
        wifi.start(queue: queue)
        general.start(queue: queue)
    }

    deinit {
        wifi.cancel()
        general.cancel()
    }

    private func recompute() {
        // Denial is checked first and on either monitor: it is the one state waiting cannot fix,
        // and it must never be rendered as "searching".
        for path in [generalPath, wifiPath] {
            if path?.status == .unsatisfied, path?.unsatisfiedReason == .localNetworkDenied {
                state = .localNetworkDenied
                return
            }
        }
        #if targetEnvironment(simulator)
        // The mock is reached over whatever the Mac uses — often Ethernet, sometimes loopback —
        // so requiring Wi-Fi here would strand every simulator session on a "no Wi-Fi" screen.
        state = generalPath?.status == .satisfied ? .dongleUp : .noDongle(generalPath?.unsatisfiedReason ?? .notAvailable)
        #else
        if CarHost.direct {
            state = wifiPath?.status == .satisfied
                ? .dongleUp : .noDongle(wifiPath?.unsatisfiedReason ?? .notAvailable)
        } else {
            // Presence is an address that either exists or does not, not a monitor's opinion of
            // whether the wire is worth offering. The dongle advertises neither gateway nor DNS
            // on purpose, so a restricted monitor can report an attached, working, fully
            // reachable dongle as unsatisfied — which is what stranded this screen on hardware.
            // `recompute` still runs on every general-path update, and unplugging the cable
            // changes the interface set, so this re-answers exactly when it needs to.
            state = CarInterface.attached
                ? .dongleUp : .noDongle(generalPath?.unsatisfiedReason ?? .notAvailable)
        }
        #endif
    }
}
