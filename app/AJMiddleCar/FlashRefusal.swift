import Foundation

/// Why a flash did not end in `ok` — one classification for both boards, host-tested.
///
/// The car's `UpdateClient.upload` and the adapter's `DongleClient.uploadFirmware` throw the
/// same `CarError` vocabulary, and the screen used to read the two differently. The car's path
/// quoted the envelope's `message` — the English sentence `docs/protocol.md` → Errors reserves
/// for a log — while the adapter's path quoted whatever Swift printed for the error, `timeout(60.0)`
/// included, as the adapter's own words. Now both paths go through `of(_:)`, and the screen
/// quotes a board only when the board spoke: its `code`, as a phrase from the strings table
/// (`L.fwFailLine`), one per code of both contracts.
public enum FlashRefusal: Equatable {
    /// The board answered with the contract's error envelope. `code` is what the screen names;
    /// `message` is the sentence the log keeps and the screen never shows.
    case envelope(code: String, message: String)
    /// The board answered an HTTP error whose body is not the envelope — something in front of
    /// it, or a firmware older than the envelope. The screen names the status.
    case http(status: Int)
    /// No board answered: no adapter, a refused connection, an expired deadline, a truncated
    /// stream — or the image never left the phone. Nothing to quote; `reason` is for the log.
    case transport(String)

    /// Every code of both contracts — the ones the strings table carries a phrase for. A code
    /// outside this set reaches the screen as the word the board said, like `L.ctlOwner` does
    /// for an owner this build does not know.
    public static let knownCodes: Set<String> =
        Set(CarErrorCode.all.map(\.rawValue) + DongleErrorCode.all.map(\.rawValue))

    /// Classify what an upload threw. Cancellation is not a refusal and is caught before this.
    /// Pure, and here rather than in the flow so the rule is host-tested — the flow's job is to
    /// catch, log and pass it on, as with `VersionReply.of`.
    public static func of(_ error: Error) -> FlashRefusal {
        guard let e = error as? CarError else { return .transport(String(describing: error)) }
        switch e {
        case .http(let status, let body):
            // Decoded by shape, not as `CarAPIError` or `DongleAPIError`: the same envelope
            // serves both boards, and a code this build does not know must survive as a word.
            if let env = try? JSONDecoder().decode(Envelope.self, from: body) {
                return .envelope(code: env.error.code, message: env.error.message ?? "")
            }
            return .http(status: status)
        // `.malformed` sits here deliberately, as in `VersionReply.of`: it is a connection that
        // closed without a parseable head, or an `NWError` that is neither an unsatisfied path
        // nor ECONNREFUSED. Neither is evidence that a board answered.
        case .noDongle, .denied, .refused, .timeout, .malformed, .truncated:
            return .transport(e.logDescription)
        }
    }

    /// What the screen quotes after «<board> ответил(а):» — `phrase` for a known code, the word
    /// itself for one this build does not know, `HTTP <status>` for a body without the
    /// envelope; nil when nothing answered, and the generic line applies to either board.
    public func quote(phrase: (String) -> String) -> String? {
        switch self {
        case .envelope(let code, _): return Self.knownCodes.contains(code) ? phrase(code) : code
        case .http(let status): return "HTTP \(status)"
        case .transport: return nil
        }
    }

    /// For the log: the envelope's code and message, a bare status, or the client's own words.
    public var logDescription: String {
        switch self {
        case .envelope(let code, let message): return "\(code): \(message)"
        case .http(let status): return "http \(status), no envelope"
        case .transport(let reason): return reason
        }
    }

    private struct Envelope: Decodable {
        struct Body: Decodable { let code: String; let message: String? }
        let error: Body
    }
}
