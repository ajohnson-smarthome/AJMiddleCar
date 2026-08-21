import Foundation

/// Reads/writes the car's wheel + motor params via GET/POST /wheel.
struct WheelClient {
    struct Params: Equatable {
        var diameterMm: Int
        var ppr: Int
        var gearX100: Int
        var quad: Int
    }

    func get() async -> Params? {
        guard let r = await CarHTTP.get("/wheel"), r.status == 200,
              let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any],
              let d = j["diameter_mm"] as? Int,
              let ppr = j["ppr"] as? Int,
              let gear = j["gear_x100"] as? Int,
              let quad = j["quad"] as? Int else { return nil }
        return Params(diameterMm: d, ppr: ppr, gearX100: gear, quad: quad)
    }

    @discardableResult
    func set(_ p: Params) async -> Bool {
        struct Body: Encodable { let diameter_mm, ppr, gear_x100, quad: Int }
        guard let body = try? JSONEncoder().encode(
            Body(diameter_mm: p.diameterMm, ppr: p.ppr, gear_x100: p.gearX100, quad: p.quad)) else {
            return false
        }
        return await CarHTTP.post("/wheel", body: body)?.status == 200
    }
}
