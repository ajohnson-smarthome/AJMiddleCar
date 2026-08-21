import Foundation

/// Single source of the car's address. Simulator builds talk to the localhost mock;
/// real-device builds talk to the car's softAP at 192.168.4.1.
enum CarHost {
    #if targetEnvironment(simulator)
    static let host = "127.0.0.1"
    static let port: UInt16 = 8080
    #else
    static let host = "192.168.4.1"
    static let port: UInt16 = 80
    #endif

    static var httpBase: String { "http://\(host):\(port)" }
    static var wsURL: String { "ws://\(host):\(port)/ws" }
    static var statusURL: String { httpBase + "/status" }
}
