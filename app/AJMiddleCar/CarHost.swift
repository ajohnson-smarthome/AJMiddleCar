import Foundation

/// Single source of the car's address. Real-device builds address the dongle at
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
    #else
    /// The bench escape hatch. Not a setting: no UI reads it, nothing persists it, and its only
    /// purpose is telling apart "the app is broken" from "the dongle is broken" during the bench
    /// sessions that follow this cutover — otherwise the most expensive question in the system,
    /// because both failures look identical from here (nothing connects). `-carHost` addresses
    /// the car directly over the phone's own Wi-Fi, exactly as the app did before the dongle
    /// existed; given nothing, the app addresses the dongle. It selects a pair, not just a host:
    /// `CarNet` reads this same `direct` flag to decide which interface to pin to, because
    /// addressing the car over the dongle's interface would fail exactly as surely as addressing
    /// the dongle over Wi-Fi — one flag, read in both places, so the two cannot disagree. It
    /// comes out once `firmware/s3/README.md`'s bench table records a drive through the relay.
    static let direct = launchArgument("-carHost") != nil

    static let host = direct ? launchArgument("-carHost")! : DongleContract.host
    /// Direct mode falls back to the car's own AP port (`80`), matching how this build addressed
    /// the car before the dongle existed. Dongle mode takes the relay's port from the contract —
    /// numerically what the car's own port already was, since the relay listens on it unchanged.
    static let port: UInt16 = direct
        ? (launchArgument("-carPort").flatMap(UInt16.init) ?? 80)
        : DongleContract.relayHttpPort
    /// Direct mode falls back to the contract's port, for a bench mock standing in for the car;
    /// dongle mode takes the relay's RT port, numerically the same value under a different name.
    static let rtPort: UInt16 = direct
        ? (launchArgument("-carRtPort").flatMap(UInt16.init) ?? CarContract.rtPort)
        : DongleContract.relayRtPort
    #endif

    static var httpBase: String { "http://\(host):\(port)" }

    private static func launchArgument(_ name: String) -> String? {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
        return args[i + 1]
    }
}
