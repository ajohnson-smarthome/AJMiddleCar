import Foundation
import Network

/// Why a call to the car did not produce an answer.
///
/// This replaces `nil`. The client layer used to return the same `nil` for connect-refused, an
/// unsatisfied path, a send error, a deadline, a truncated stream and an unparseable head — and
/// every call site flattened that to `if let v = await X().get() { … }` with no `else`, so a car
/// that was not answering was indistinguishable from a car that answered with defaults.
enum CarError: Error, Equatable {
    /// The path cannot carry the request at all; `reason` says which of the four ways.
    case noWiFi(NWPath.UnsatisfiedReason)
    /// Local-network access was denied. No wait fixes it — only the user, in Settings.
    case denied
    case refused
    case timeout(TimeInterval)
    case http(status: Int, body: Data)
    case malformed(String)
    case truncated(got: Int, want: Int)

    /// Map a `Network.framework` failure. The path-level reasons are the ones worth separating:
    /// they are the difference between "wait" and "the user must do something".
    static func from(_ error: NWError?, path: NWPath?) -> CarError {
        if let path, path.status == .unsatisfied {
            return path.unsatisfiedReason == .localNetworkDenied ? .denied : .noWiFi(path.unsatisfiedReason)
        }
        if case .posix(let code)? = error, code == .ECONNREFUSED { return .refused }
        if let error { return .malformed(String(describing: error)) }
        return .refused
    }

    /// For logs only — user-facing text is localized in the views that show it.
    var logDescription: String {
        switch self {
        case .noWiFi(let r): return "no wifi (\(r))"
        case .denied: return "local network denied"
        case .refused: return "refused"
        case .timeout(let s): return "timeout after \(s)s"
        case .http(let status, _): return "http \(status)"
        case .malformed(let what): return "malformed: \(what)"
        case .truncated(let got, let want): return "truncated \(got)/\(want)"
        }
    }
}
