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
    static func params(webSocket: Bool) -> NWParameters {
        let p = NWParameters.tcp
        if webSocket {
            let ws = NWProtocolWebSocket.Options()
            ws.autoReplyPing = true
            p.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)
        }
        #if !targetEnvironment(simulator)
        // Deliberately absent in the simulator: there the car is the mock on 127.0.0.1, and
        // loopback is not Wi-Fi — pinning would make every simulator build unable to connect.
        p.requiredInterfaceType = .wifi
        p.prohibitedInterfaceTypes = [.cellular]
        #endif
        return p
    }

    static func endpoint() -> NWEndpoint {
        NWEndpoint.hostPort(host: NWEndpoint.Host(CarHost.host),
                            port: NWEndpoint.Port(rawValue: CarHost.port)!)
    }

    /// The WebSocket needs its path, which a host/port endpoint cannot carry.
    static func wsEndpoint() -> NWEndpoint {
        NWEndpoint.url(URL(string: CarHost.wsURL)!)
    }
}
