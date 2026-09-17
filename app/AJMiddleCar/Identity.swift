import Foundation

/// GET /version — who a board is and what it runs. The one document in either contract whose
/// shape never changes: five fields, this order, nothing else, so this decoder is hand-written
/// and frozen rather than generated. A board that answers 404 predates the endpoint and is
/// updated; that is `VersionReply.absent`, not a decode.
public struct DeviceVersion: Codable, Equatable {
    /// `ajmiddlecar` or `ajdongle` — the first thing the gate checks, before any other field
    /// of any document is believed.
    public let device: String
    /// The version as the build prints it, `v1.0+879` (dev builds add `-<n>-g<sha>[-dirty]`).
    public let fw: String
    /// The number after `+` in `fw`, parsed by the firmware; -1 when `fw` carries none.
    public let build: Int
    /// The protocol number of everything else this board serves — the car's `car-api.proto`,
    /// the adapter's `dongle-api.proto`. Not ours → the board is newer than this app.
    public let proto: Int
    /// The bootloader reverted the last update; sticky until the next successful OTA.
    public let rolled_back: Bool

    public init(device: String, fw: String, build: Int, proto: Int, rolled_back: Bool) {
        self.device = device; self.fw = fw; self.build = build; self.proto = proto
        self.rolled_back = rolled_back
    }
}

/// What one read of `/version` produced. Five situations with five different things to say;
/// the flow classifies, `VersionRule` decides.
public enum VersionReply {
    /// The document, decoded.
    case version(DeviceVersion)
    /// 404: a board older than the endpoint. Treated as behind — the ordinary forced update
    /// takes it across, and its `POST /ota` has always existed.
    case absent
    /// Nothing answered: no cable, a refused connection, a deadline that expired with no bytes.
    case silent
    /// Something answered and it was not usable: an HTTP error other than 404, a truncated
    /// stream, or a body that is not this document. A device is there and talking.
    case faulty
    /// iOS refused to let the request leave the phone: local-network access is denied.
    case denied

    /// Read a body as the frozen document, else as a fault. Pure.
    public static func decode(_ data: Data) -> VersionReply {
        if let v = try? JSONDecoder().decode(DeviceVersion.self, from: data) { return .version(v) }
        return .faulty
    }

    /// Classify what a client's GET threw. Pure, and here rather than in the flow so the rule
    /// is host-tested — the flow's job is to catch, log and pass it on.
    public static func of(_ error: Error) -> VersionReply {
        if let e = error as? CarError {
            switch e {
            case .http(let status, _): return status == 404 ? .absent : .faulty
            case .truncated: return .faulty
            case .denied: return .denied
            // `.malformed` sits on this side deliberately: `CarError.from` uses it for any
            // `NWError` that is neither an unsatisfied path nor ECONNREFUSED, and the request
            // types use it for a connection that closed without a parseable head. Neither is
            // evidence that a board answered.
            case .noDongle, .refused, .timeout, .malformed: return .silent
            }
        }
        // A `DecodingError` is a complete body this build could not read — something answered.
        // Anything else, a cancellation most of all, is evidence of nothing.
        return error is DecodingError ? .faulty : .silent
    }
}
