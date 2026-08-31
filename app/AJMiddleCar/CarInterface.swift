import Foundation
import Network

/// Which wire the car's traffic leaves on — discovered, never assumed.
///
/// This file exists because the assumption cost a bench session. The app used to pin every socket
/// to an *interface type* the spec guessed iOS would report for the dongle's CDC-NCM device
/// (`.wiredEthernet`). It does not. With the dongle attached and working perfectly — the phone
/// held a DHCP lease from it (`192.168.7.2`), and Safari loaded both the dongle's own `/status`
/// and the car's through the relay — every socket the app opened sat in `.waiting` until it timed
/// out, and the app showed "no adapter" with the cable plugged in. The pinned type was the only
/// difference between Safari and us.
///
/// So the type is no longer guessed at, or even consulted. The dongle's interface is **the one
/// holding an address on the dongle's own subnet** — a definition rather than a heuristic, and one
/// this project controls end to end: `DongleContract.host` is generated from
/// `contract/dongle-api.json`, and the dongle's DHCP server hands the phone the neighbouring
/// address out of that same /24. Whatever iOS decides to call that interface — `.wiredEthernet`,
/// `.other`, or something not yet invented — it is found by the address it carries.
///
/// This retires U1. There is no longer a question to answer about interface types.
enum CarInterface {
    /// The /24 an IPv4 address belongs to, as a string prefix: `192.168.7.1` → `192.168.7.`
    ///
    /// A prefix rather than a parsed mask because that is all the comparison needs, and because a
    /// malformed address must fail here rather than silently match everything: an empty prefix
    /// would make `hasPrefix` true for every interface on the phone.
    static func subnetPrefix(of address: String) -> String? {
        let parts = address.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4,
              parts.allSatisfy({ !$0.isEmpty && $0.allSatisfy(\.isNumber) }) else { return nil }
        return parts.prefix(3).joined(separator: ".") + "."
    }

    /// Names of the local interfaces carrying an IPv4 address on the given address's subnet.
    ///
    /// `getifaddrs` rather than `NWPath.availableInterfaces` because this must answer even when
    /// Network.framework declines to offer the wire at all — see `current` for why that is a real
    /// possibility here and not paranoia.
    static func attachedNames(onSubnetOf address: String = DongleContract.host) -> [String] {
        guard let prefix = subnetPrefix(of: address) else { return [] }
        var names: [String] = []
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return names }
        defer { freeifaddrs(head) }
        for ifa in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let sa = ifa.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            var storage = sockaddr_in()
            memcpy(&storage, sa, MemoryLayout<sockaddr_in>.size)
            var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            guard inet_ntop(AF_INET, &storage.sin_addr, &text, socklen_t(INET_ADDRSTRLEN)) != nil
            else { continue }
            if String(cString: text).hasPrefix(prefix) {
                names.append(String(cString: ifa.pointee.ifa_name))
            }
        }
        return names
    }

    /// Is the dongle attached at all? The presence question `CarPath` renders, answered by an
    /// address that either exists or does not, rather than by a monitor's opinion of whether the
    /// wire is useful for general traffic.
    static var attached: Bool { !attachedNames().isEmpty }

    /// Started once, on first use. `CarTransport` keeps its own queue the same way.
    private static let monitor: NWPathMonitor = {
        let m = NWPathMonitor()
        m.start(queue: DispatchQueue(label: "car.interface"))
        return m
    }()

    /// The concrete interface to pin to, or `nil`.
    ///
    /// `nil` is not a failure and must not be treated as one. The dongle deliberately advertises
    /// neither a gateway nor a DNS server (`firmware/s3/main/usb_net.c` — that is what keeps it
    /// from stealing the host's default route, and it is proven behaviour on macOS), so iOS is
    /// entitled to decline to list a wire it considers unusable for general traffic. When that
    /// happens the dongle is still there and still reachable: `CarNet` simply does not pin, and
    /// routing delivers an on-link address by itself. That is not a hope — it is what Safari did
    /// on the bench, to both `192.168.7.1:8080` and the car behind the relay, while the pinned
    /// app next to it could not open a socket at all.
    static var current: NWInterface? {
        let names = Set(attachedNames())
        guard !names.isEmpty else { return nil }
        return monitor.currentPath.availableInterfaces.first { names.contains($0.name) }
    }
}
