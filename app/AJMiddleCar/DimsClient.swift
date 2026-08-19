import Foundation

/// Reads/writes the car's physical dimensions via GET/POST /dims.
/// GET returns JSON; POST sends two space-separated ints (mirrors the firmware).
struct DimsClient {
    struct Params: Equatable {
        var trackMm: Int
        var wheelbaseMm: Int
    }

    func get() async -> Params? {
        guard let url = URL(string: CarHost.httpBase + "/dims") else { return nil }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let track = j["track_mm"] as? Int,
              let base = j["wheelbase_mm"] as? Int else { return nil }
        return Params(trackMm: track, wheelbaseMm: base)
    }

    @discardableResult
    func set(_ p: Params) async -> Bool {
        guard let url = URL(string: CarHost.httpBase + "/dims") else { return false }
        var req = URLRequest(url: url)
        struct Body: Encodable { let track_mm, wheelbase_mm: Int }
        req.httpMethod = "POST"
        req.httpBody = try? JSONEncoder().encode(Body(track_mm: p.trackMm, wheelbase_mm: p.wheelbaseMm))
        guard let (_, resp) = try? await URLSession.shared.data(for: req) else { return false }
        return (resp as? HTTPURLResponse)?.statusCode == 200
    }
}
