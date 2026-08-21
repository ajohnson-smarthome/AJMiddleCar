import Foundation

/// Reads/writes the car's acceleration-ramp time (ms) via GET/POST /ramp.
struct RampClient {
    func get() async -> Int? {
        guard let r = await CarHTTP.get("/ramp"), r.status == 200,
              let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any],
              let v = j["ramp_ms"] as? Int else { return nil }
        return v
    }

    @discardableResult
    func set(_ ms: Int) async -> Bool {
        struct Body: Encodable { let ramp_ms: Int }
        guard let body = try? JSONEncoder().encode(Body(ramp_ms: ms)) else { return false }
        return await CarHTTP.post("/ramp", body: body)?.status == 200
    }
}
