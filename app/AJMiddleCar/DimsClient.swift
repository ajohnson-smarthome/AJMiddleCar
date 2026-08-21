import Foundation

/// Reads/writes the car's physical dimensions via GET/POST /dims.
struct DimsClient {
    struct Params: Equatable {
        var trackMm: Int
        var wheelbaseMm: Int
    }

    func get() async -> Params? {
        guard let r = await CarHTTP.get("/dims"), r.status == 200,
              let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any],
              let track = j["track_mm"] as? Int,
              let base = j["wheelbase_mm"] as? Int else { return nil }
        return Params(trackMm: track, wheelbaseMm: base)
    }

    @discardableResult
    func set(_ p: Params) async -> Bool {
        struct Body: Encodable { let track_mm, wheelbase_mm: Int }
        guard let body = try? JSONEncoder().encode(
            Body(track_mm: p.trackMm, wheelbase_mm: p.wheelbaseMm)) else { return false }
        return await CarHTTP.post("/dims", body: body)?.status == 200
    }
}
