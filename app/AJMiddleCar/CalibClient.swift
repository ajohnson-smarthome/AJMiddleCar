import Foundation

/// The car's calibration endpoints. Not a config domain — the wizard's protocol is three calls,
/// not one record — so it stays hand-written while the five domains are generic.
@MainActor
final class CalibClient {
    private let transport: CarTransport

    init(transport: CarTransport = .shared) { self.transport = transport }

    func fetchCalibrated() async throws -> Bool {
        let data = try await transport.get("/calib")
        guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let calibrated = j["calibrated"] as? Bool else {
            throw CarError.malformed("/calib without a calibrated flag")
        }
        return calibrated
    }

    /// Spin one motor pair, and **say so when it did not happen**.
    ///
    /// This returned `Void` before, so a POST that never reached the car looked exactly like a
    /// wheel that turned. Four taps on a car that answered nothing produced a table
    /// `calibration_valid` cannot reject, and the car then drove with swapped wheels while
    /// reporting `calibrated: true`.
    func spin(pair: Int, dir: Int) async throws {
        try await post("/calib/spin", body: #"{"pair":\#(pair),"dir":\#(dir)}"#)
    }

    func save(body: String) async throws {
        try await post("/calib/save", body: body)
    }

    private func post(_ path: String, body: String) async throws {
        _ = try await transport.post(path, body: Data(body.utf8))
    }
}
