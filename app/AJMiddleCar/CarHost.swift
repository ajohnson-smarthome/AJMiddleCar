import Foundation

/// Single source of the car's address. Real-device builds talk to the car's softAP; simulator
/// builds talk to the mock.
enum CarHost {
    #if targetEnvironment(simulator)
    /// The mock binds `0.0.0.0`, so it is reachable both on loopback and on the Mac's LAN
    /// address. `-carHost 192.168.1.20` points the simulator at the latter, which is the only
    /// way a simulator session exercises anything resembling a real path.
    static let host = launchArgument("-carHost") ?? "127.0.0.1"
    static let port: UInt16 = launchArgument("-carPort").flatMap(UInt16.init) ?? 8080
    #else
    static let host = CarContract.host
    static let port: UInt16 = 80
    #endif

    /// The real-time channel is on the contract's port on both builds — only REST moves.
    static let rtPort: UInt16 = CarContract.rtPort

    static var httpBase: String { "http://\(host):\(port)" }

    #if targetEnvironment(simulator)
    private static func launchArgument(_ name: String) -> String? {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
        return args[i + 1]
    }
    #endif
}
