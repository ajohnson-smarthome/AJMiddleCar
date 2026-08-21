import Foundation
import Network

/// Interface and permission truth, from two `NWPathMonitor`s.
///
/// Two, because the car's network is the one case where the general path being unsatisfied is
/// *normal*: iOS demotes a Wi-Fi with no internet out of general routing while the interface
/// itself keeps working, which is exactly the situation the app spends its life in. The
/// Wi-Fi-restricted monitor says whether the car is reachable at all; the general monitor is
/// where local-network denial shows up.
///
/// Nothing here used to be read. Wi-Fi off, local network denied, wrong network and a powered-off
/// car were one indistinguishable radar.
@MainActor
final class CarPath: ObservableObject {
    @Published private(set) var state: PathState = .noWifi(.notAvailable)

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
        state = generalPath?.status == .satisfied ? .wifiUp : .noWifi(generalPath?.unsatisfiedReason ?? .notAvailable)
        #else
        if wifiPath?.status == .satisfied {
            state = .wifiUp
        } else {
            state = .noWifi(wifiPath?.unsatisfiedReason ?? .notAvailable)
        }
        #endif
    }
}
