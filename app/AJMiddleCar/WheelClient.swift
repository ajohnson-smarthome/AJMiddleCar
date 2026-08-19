import Foundation

/// Reads/writes the car's wheel + motor params via GET/POST /wheel.
/// GET returns JSON; POST sends four space-separated ints (mirrors the firmware).
struct WheelClient {
    struct Params: Equatable {
        var diameterMm: Int
        var ppr: Int
        var gearX100: Int
        var quad: Int
    }

    func get() async -> Params? {
        guard let url = URL(string: CarHost.httpBase + "/wheel") else { return nil }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let d = j["diameter_mm"] as? Int,
              let ppr = j["ppr"] as? Int,
              let gear = j["gear_x100"] as? Int,
              let quad = j["quad"] as? Int else { return nil }
        return Params(diameterMm: d, ppr: ppr, gearX100: gear, quad: quad)
    }

    @discardableResult
    func set(_ p: Params) async -> Bool {
        guard let url = URL(string: CarHost.httpBase + "/wheel") else { return false }
        struct Body: Encodable { let diameter_mm, ppr, gear_x100, quad: Int }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.httpBody = try? JSONEncoder().encode(
            Body(diameter_mm: p.diameterMm, ppr: p.ppr, gear_x100: p.gearX100, quad: p.quad))
        guard let (_, resp) = try? await URLSession.shared.data(for: req) else { return false }
        return (resp as? HTTPURLResponse)?.statusCode == 200
    }
}
