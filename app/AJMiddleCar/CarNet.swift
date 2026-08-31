import Foundation
import Network

/// The one place that decides how a socket to the car is opened.
///
/// The dongle joins the car's access point on the phone's behalf, over its own USB interface —
/// the phone keeps its own Wi-Fi, with a route to the internet, for as long as the dongle is
/// attached. That is the whole point of the device, and it is also why pinning still matters:
/// with two live interfaces to the world (the phone's own Wi-Fi/cellular, and the dongle), a
/// socket left unpinned could satisfy itself over the wrong one and silently talk to nothing.
/// Pinning to the dongle's interface is what keeps the car reachable on exactly one named wire.
///
/// `URLSession` cannot express that binding, which is why the car's traffic runs on
/// `Network.framework`. Both transports build their parameters here so the rule cannot drift.
///
/// *Which* wire that is used to be a guess about interface types, and the guess was wrong on
/// hardware. `CarInterface` now answers it by address instead; this file only applies the answer.
enum CarNet {
    /// REST: `/status`, `/calib*`, `/ota` and the five config domains.
    static func tcpParams() -> NWParameters { pinToCarInterface(.tcp) }

    /// The real-time channel. Control and telemetry are state with last-wins semantics, which is
    /// what a datagram is: a lost command costs one 10 Hz tick instead of head-of-line-blocking
    /// the stream for a 1.5 s TCP retransmission — five times the car's watchdog deadline.
    static func udpParams() -> NWParameters { pinToCarInterface(.udp) }

    /// Pin to the dongle's wire — the concrete one `CarInterface` found by address, not a type
    /// anybody guessed. When it cannot be identified the socket is left unpinned rather than
    /// pinned to nothing: see `CarInterface.current` for why that case is legitimate and why
    /// routing still delivers. Cellular stays prohibited either way — the car is never out there.
    private static func pinToCarInterface(_ p: NWParameters) -> NWParameters {
        #if !targetEnvironment(simulator)
        if CarHost.direct {
            // The bench escape hatch reaches the car over the phone's own Wi-Fi, where the
            // interface type is known rather than assumed, so pinning by type is still right.
            p.requiredInterfaceType = .wifi
        } else if let wire = CarInterface.current {
            p.requiredInterface = wire
        }
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
