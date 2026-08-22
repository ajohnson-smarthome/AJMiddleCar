import Foundation
import Network

/// The one place that decides how a socket to the car is opened.
///
/// The car is an access point with no route to the internet, and iOS reacts to that by dropping
/// Wi-Fi out of the general network path — the interface stays up, Settings still says
/// "connected", and every request quietly times out. Measured on the bench: the general path went
/// `unsatisfied` forty seconds after launch while a Wi-Fi-restricted path stayed `satisfied`, and
/// a connection bound to Wi-Fi reached the car in 21 ms.
///
/// `URLSession` cannot express that binding, which is why the car's traffic runs on
/// `Network.framework`. Both transports build their parameters here so the rule cannot drift.
enum CarNet {
    /// REST: `/status`, `/calib*`, `/ota` and the five config domains.
    static func tcpParams() -> NWParameters { pinToWiFi(.tcp) }

    /// The real-time channel. Control and telemetry are state with last-wins semantics, which is
    /// what a datagram is: a lost command costs one 10 Hz tick instead of head-of-line-blocking
    /// the stream for a 1.5 s TCP retransmission — five times the car's watchdog deadline.
    static func udpParams() -> NWParameters { pinToWiFi(.udp) }

    private static func pinToWiFi(_ p: NWParameters) -> NWParameters {
        #if !targetEnvironment(simulator)
        p.requiredInterfaceType = .wifi
        p.prohibitedInterfaceTypes = [.cellular]
        #else
        // Deliberately absent in the simulator: the car there is the mock, which may be on
        // loopback (not an interface type at all) or on the Mac's LAN. Pinning would make every
        // simulator build unable to connect to either.
        _ = p
        #endif
        return p
    }

    /// The REST endpoint.
    static func endpoint() -> NWEndpoint {
        NWEndpoint.hostPort(host: NWEndpoint.Host(CarHost.host),
                            port: NWEndpoint.Port(rawValue: CarHost.port)!)
    }

    /// The real-time endpoint: same host, the contract's UDP port.
    static func rtEndpoint() -> NWEndpoint {
        NWEndpoint.hostPort(host: NWEndpoint.Host(CarHost.host),
                            port: NWEndpoint.Port(rawValue: CarHost.rtPort)!)
    }
}
