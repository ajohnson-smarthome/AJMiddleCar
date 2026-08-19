import Foundation

/// Single source of the car's address. Simulator builds talk to the localhost mock;
/// real-device builds talk to the car's softAP at 192.168.4.1.
enum CarHost {
    #if targetEnvironment(simulator)
    static let httpBase = "http://127.0.0.1:8080"
    static let wsURL    = "ws://127.0.0.1:8080/ws"
    #else
    static let httpBase = "http://192.168.4.1"
    static let wsURL    = "ws://192.168.4.1/ws"
    #endif
    static var statusURL: String { httpBase + "/status" }
}
