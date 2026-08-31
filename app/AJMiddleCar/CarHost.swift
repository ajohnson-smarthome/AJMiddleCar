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
    /// both `CarNet`'s socket pinning and `CarPath`'s presence test branch on this same `direct`
    /// flag, so host and wire cannot disagree — direct mode pins to Wi-Fi by type, where the type
    /// is known; dongle mode asks `CarInterface` which wire carries the dongle's address. It comes
    /// It was meant to come out once `firmware/s3/README.md`'s bench table recorded a drive
    /// through the relay. That row is now filled in — the app reached the car through the dongle
    /// on 2026-08-31 — and this stays anyway, deliberately: the dongle's own OTA cycle and its
    /// rollback proof are still unrun, and those are precisely the tests that can leave it unable
    /// to relay. While that is true, this flag is the only way to keep testing the app against
    /// the car. It comes out when that debt closes, not before.
    static let direct = launchArgument("-carHost") != nil

    static let host = direct ? launchArgument("-carHost")! : DongleContract.host
    /// `-carPort` overrides the port only in direct mode, alongside `-carHost` — the pairing is
    /// both arguments together or neither. The fallback is `DongleContract.relayHttpPort` either
    /// way, and for the same reason in both modes: the relay listens on the car's own REST port
    /// unchanged, and `car-api.json` carries no HTTP port of its own to spell instead — the
    /// dongle's contract is the only non-literal source for that number, direct or not.
    private static var directPort: UInt16? { direct ? launchArgument("-carPort").flatMap(UInt16.init) : nil }
    static let port: UInt16 = directPort ?? DongleContract.relayHttpPort
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
