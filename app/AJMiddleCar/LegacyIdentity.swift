import Foundation

/// Who answered `/status`, read the way BOTH formats spell it. The one bridge across the
/// v1→v2 flag day: a v1 car does not answer a v2 hello at all, and a v1 dongle's `/status`
/// does not decode as a v2 document, so without this the app could never learn that a board
/// is behind — and could never push the update that fixes it.
///
/// The v1 keys are literals on purpose: they are no longer in the contract, and the day no
/// board in the field speaks v1 this file is deleted, not maintained.
public struct LegacyIdentity: Equatable {
    let device: String
    let fw: String

    public static func parse(_ data: Data) -> LegacyIdentity? {
        guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        // v2: "device":{"id":…,"fw":…}
        if let d = j["device"] as? [String: Any], let id = d["id"] as? String, let fw = d["fw"] as? String {
            return LegacyIdentity(device: id, fw: fw)
        }
        // v1: "device":…,"fw":… at the top level
        if let id = j["device"] as? String, let fw = j["fw"] as? String {
            return LegacyIdentity(device: id, fw: fw)
        }
        return nil
    }
}
