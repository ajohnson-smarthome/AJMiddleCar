// Host test for CarInterface — how the app decides which wire the car is on.
//
// This is the module that replaced a guess about `NWInterface.InterfaceType` after the guess
// stranded the app on "no adapter" with a working dongle attached. The guess was untestable off
// a device, which is most of why it survived to the bench; the definition that replaced it —
// "the interface holding an address on the dongle's subnet" — is testable right here, because
// every machine has a loopback interface at a known address to find.
//
// `sources` lists only CarInterface.swift; `DongleContract` comes from Generated/DongleAPI.swift,
// which tools/test-all.sh puts on every Swift test's compile line.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// A prefix, not a parsed mask — but a strict one. The danger this guards is an empty or partial
// prefix, which `hasPrefix` would match against every address on the phone.
check(CarInterface.subnetPrefix(of: "192.168.7.1") == "192.168.7.", "the dongle's own /24")
check(CarInterface.subnetPrefix(of: "10.0.0.5") == "10.0.0.", "a ten-net address")
check(CarInterface.subnetPrefix(of: "127.0.0.1") == "127.0.0.", "loopback")
check(CarInterface.subnetPrefix(of: "192.168.7") == nil, "three octets is not an address")
check(CarInterface.subnetPrefix(of: "192.168.7.1.9") == nil, "five octets is not an address")
check(CarInterface.subnetPrefix(of: "") == nil, "the empty string must not yield a prefix")
check(CarInterface.subnetPrefix(of: "a.b.c.d") == nil, "letters are not octets")
check(CarInterface.subnetPrefix(of: "192..7.1") == nil, "an empty octet is not an octet")

// The contract's own value has to survive the parser, or the whole mechanism is inert.
check(CarInterface.subnetPrefix(of: DongleContract.host) == "192.168.7.",
      "the generated dongle address parses")

// The getifaddrs walk itself: loopback is the one interface guaranteed to exist, at a known
// address, on every machine this can run on. If this fails, address-based discovery is broken and
// no amount of correct prefix arithmetic saves it.
let loopback = CarInterface.attachedNames(onSubnetOf: "127.0.0.1")
check(loopback.contains("lo0"), "127.0.0.0/24 is carried by lo0, found: \(loopback)")

// TEST-NET-3 (RFC 5737) is reserved for documentation and must never be configured on a host.
check(CarInterface.attachedNames(onSubnetOf: "203.0.113.1").isEmpty,
      "a reserved documentation subnet matches nothing")

// A malformed address must find nothing rather than everything — the failure mode that would
// pin every socket to whatever interface happened to be enumerated first.
check(CarInterface.attachedNames(onSubnetOf: "nonsense").isEmpty,
      "a malformed address matches no interface")

if failures == 0 { print("carinterface: all checks passed") }
exit(failures == 0 ? 0 : 1)
