import Foundation

/// Single source of the car's address. Device builds address the dongle at
/// `DongleContract.host`, which relays to the car; simulator builds talk to the mock.
enum CarHost {
    #if targetEnvironment(simulator)
    /// The mock binds `0.0.0.0`, so it is reachable both on loopback and on the Mac's LAN
    /// address. `-carHost 192.168.1.20` points the simulator at the latter, which is the only
    /// way a simulator session exercises anything resembling a real path.
    static let host = launchArgument("-carHost") ?? "127.0.0.1"
    static let port: UInt16 = launchArgument("-carPort").flatMap(UInt16.init) ?? 8080
    /// The real-time channel is on the contract's port unless a mock says otherwise: a second
    /// mock on a spare RT port (which is how `tools/test-all.sh` runs one) is only reachable if
    /// this can be pointed at it.
    static let rtPort: UInt16 = launchArgument("-carRtPort").flatMap(UInt16.init) ?? CarContract.rtPort

    private static func launchArgument(_ name: String) -> String? {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
        return args[i + 1]
    }
    #else
    /// On a device the car is reached through the dongle, and only through it. There is no
    /// setting and no launch argument that says otherwise: the direct path over the phone's own
    /// Wi-Fi — the one the app was born with — was retired once the dongle had relayed a drive
    /// (2026-08-31) and survived its own OTA cycle and rollback on the bench. The ports are the
    /// car's native numbers, which the relay listens on unchanged, so `car-api.json` carries no
    /// HTTP port of its own and the dongle's contract is the one source for both.
    static let host = DongleContract.host
    static let port: UInt16 = DongleContract.relayHttpPort
    static let rtPort: UInt16 = DongleContract.relayRtPort
    #endif

    static var httpBase: String { "http://\(host):\(port)" }
}
