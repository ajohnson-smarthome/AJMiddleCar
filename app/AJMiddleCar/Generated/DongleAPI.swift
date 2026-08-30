// generated from contract/dongle-api.json by tools/gen_contract.py - do not edit

public enum DongleContract {
    public static let device = "ajdongle"
    public static let host = "192.168.7.1"
    public static let port: UInt16 = 8080

    public static let statusPath = "/status"
    public static let netPath = "/net"
    public static let otaPath = "/ota"

    public static let ssidMin = 1
    public static let ssidMax = 32
    public static let passMin = 8
    public static let passMax = 63

    public static let ssidField = "ssid"
    public static let passwordField = "password"
    public static let configuredField = "configured"
}

/// The keys `/status` uses. Named here so the app never spells one as a literal.
public enum DongleStatusKey {
    public static let device = "device"
    public static let fw = "fw"
    public static let idf = "idf"
    public static let usb = "usb"
    public static let rollback = "rollback"
    public static let net = "net"
    public static let netSsid = "ssid"
    public static let netState = "state"
    public static let netRssi = "rssi"
}

/// What the dongle's radio is doing, as `/status` reports it.
public enum DongleNetState {
    public static let idle = "idle"
    public static let joining = "joining"
    public static let connected = "connected"
    public static let failed = "failed"
    public static let all = ["idle", "joining", "connected", "failed"]
}

/// What the dongle's USB link is doing, as `/status` reports it.
public enum DongleUsbState {
    public static let up = "up"
    public static let all = ["up"]
}
