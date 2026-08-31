import Foundation
import Network

/// Interface and permission truth, from two `NWPathMonitor`s.
///
/// Two, because the dongle's interface and the phone's general path now answer different
/// questions. The dongle-restricted monitor is the only one that can say whether the car is
/// reachable at all — its interface appears when the cable goes in and disappears when it comes
/// out, exactly the presence-or-absence question `CarPath` exists to answer. The general monitor
/// no longer needs watching for *that*: with the dongle attached the phone keeps its own Wi-Fi,
/// with internet, throughout, so the general path stays satisfied the whole time. What it still
/// carries is local-network denial, which is why both monitors are still checked for it.
///
/// Nothing here used to be read. Wi-Fi off, local network denied, wrong network and a powered-off
/// car were one indistinguishable radar.
@MainActor
final class CarPath: ObservableObject {
    @Published private(set) var state: PathState = .noDongle(.notAvailable)

    /// Restricted to `CarNet.carInterface` — normally the dongle's interface, or Wi-Fi under the
    /// bench escape hatch (`CarHost.direct`); see that constant for why the choice is folded
    /// there rather than read from `CarHost` directly.
    private let dongle = NWPathMonitor(requiredInterfaceType: CarNet.carInterface)
    private let general = NWPathMonitor()
    private let queue = DispatchQueue(label: "car.path")
    private var donglePath: NWPath?
    private var generalPath: NWPath?

    init() {
        dongle.pathUpdateHandler = { [weak self] p in
            Task { @MainActor in self?.donglePath = p; self?.recompute() }
        }
        general.pathUpdateHandler = { [weak self] p in
            Task { @MainActor in self?.generalPath = p; self?.recompute() }
        }
        dongle.start(queue: queue)
        general.start(queue: queue)
    }

    deinit {
        dongle.cancel()
        general.cancel()
    }

    private func recompute() {
        // Denial is checked first and on either monitor: it is the one state waiting cannot fix,
        // and it must never be rendered as "searching".
        for path in [generalPath, donglePath] {
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
        if donglePath?.status == .satisfied {
            state = .dongleUp
        } else {
            state = .noDongle(donglePath?.unsatisfiedReason ?? .notAvailable)
        }
        #endif
    }
}
