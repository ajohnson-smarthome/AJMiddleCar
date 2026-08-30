// generated from contract/dongle-api.json by tools/gen_dongle.py - do not edit

public enum DongleContract {
    public static let device = "ajdongle"
    public static let host = "192.168.7.1"
    public static let port: UInt16 = 8080

    public static let statusPath = "/status"
    public static let netPath = "/net"

    public static let ssidMin = 1
    public static let ssidMax = 32
    public static let passMin = 8
    public static let passMax = 63

    public static let ssidField = "ssid"
    public static let passwordField = "password"
    public static let configuredField = "configured"
}

/// What the dongle's radio is doing, as `/status` reports it.
public enum DongleNetState {
    public static let idle = "idle"
    public static let joining = "joining"
    public static let connected = "connected"
    public static let failed = "failed"
    public static let all = ["idle", "joining", "connected", "failed"]
}
