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
enum CarNet {
    /// **U1, unresolved.** The spec assumes the dongle's CDC-NCM interface reports to
    /// `Network.framework` as `.wiredEthernet`; nobody has looked. The other candidate is
    /// `.other`, which is where iOS parks USB network hardware it does not recognise as
    /// Ethernet proper. This cannot be settled from the simulator — there is no USB there — only
    /// from a device: the bench step is Task 3 Step 4 in
    /// `docs/superpowers/plans/2026-08-31-dongle-p5-app.md`, a throwaway build that logs
    /// `NWPath.availableInterfaces` with the dongle attached. Get this wrong and nothing fails
    /// loudly: every socket the app opens pins to an interface type nothing on the phone ever
    /// reports, so every connection sits in `.waiting` and times out — silence indistinguishable
    /// from a broken relay.
    static let dongleInterface: NWInterface.InterfaceType = .wiredEthernet

    /// The interface actually pinned to and monitored — `dongleInterface`, unless the bench
    /// escape hatch (`CarHost.direct`) says the car is addressed directly over the phone's own
    /// Wi-Fi. Folded into one value *here*, not in `CarHost`, because `CarPath`'s monitor needs
    /// it too and `CarHost.direct` does not exist on the simulator branch — this file already
    /// owns the `#if` both call sites have to agree with. `pinToCarInterface` and `CarPath` are
    /// the only two readers; a socket pinned to one candidate while the monitor watches the
    /// other is exactly the split this constant exists to prevent.
    static var carInterface: NWInterface.InterfaceType {
        #if !targetEnvironment(simulator)
        return CarHost.direct ? .wifi : dongleInterface
        #else
        // Never actually consulted for pinning or for `state` on this branch (both go through
        // `generalPath` instead — see `CarPath`), but `CarPath`'s `dongle` monitor is a stored
        // property created unconditionally, so this still has to return something that compiles.
        return dongleInterface
        #endif
    }

    /// REST: `/status`, `/calib*`, `/ota` and the five config domains.
    static func tcpParams() -> NWParameters { pinToCarInterface(.tcp) }

    /// The real-time channel. Control and telemetry are state with last-wins semantics, which is
    /// what a datagram is: a lost command costs one 10 Hz tick instead of head-of-line-blocking
    /// the stream for a 1.5 s TCP retransmission — five times the car's watchdog deadline.
    static func udpParams() -> NWParameters { pinToCarInterface(.udp) }

    private static func pinToCarInterface(_ p: NWParameters) -> NWParameters {
        #if !targetEnvironment(simulator)
        p.requiredInterfaceType = carInterface
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
