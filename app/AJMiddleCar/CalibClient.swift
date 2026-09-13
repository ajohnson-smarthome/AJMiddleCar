import Foundation

/// The car's calibration endpoints. Not a config domain — the wizard's protocol is three calls,
/// not one record — so it stays hand-written while the five domains are generic.
@MainActor
final class CalibClient {
    private let transport: CarTransport

    init(transport: CarTransport = .shared) { self.transport = transport }

    func fetch() async throws -> Calibration {
        try JSONDecoder().decode(Calibration.self, from: try await transport.get(CarContract.calibrationPath))
    }

    /// Spin one motor pair, and **say so when it did not happen**: a POST that never reached
    /// the car must not look like a wheel that turned.
    func spin(pair: Int, direction: CalibDirection) async throws {
        let body = try JSONEncoder().encode(SpinBody(pair: pair, direction: direction))
        _ = try await transport.post(CarContract.spinPath, body: body)
    }

    /// Save the table; the car answers with the table as now held.
    func save(_ wheels: [CalibWheel]) async throws -> Calibration {
        let body = try JSONEncoder().encode(SaveBody(wheels: wheels))
        return try JSONDecoder().decode(Calibration.self, from: try await transport.post(CarContract.calibrationPath, body: body))
    }

    private struct SpinBody: Encodable { let pair: Int; let direction: CalibDirection }
    private struct SaveBody: Encodable { let wheels: [CalibWheel] }
}
