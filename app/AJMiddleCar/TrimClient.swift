import Foundation

/// Reads/writes the car's straight-line trim (pct, -30..30) via GET/POST /trim.
struct TrimClient {
    func get() async -> Int? {
        guard let r = await CarHTTP.get("/trim"), r.status == 200,
              let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any],
              let v = j["trim_pct"] as? Int else { return nil }
        return v
    }

    @discardableResult
    func set(_ pct: Int) async -> Bool {
        struct Body: Encodable { let trim_pct: Int }
        guard let body = try? JSONEncoder().encode(Body(trim_pct: pct)) else { return false }
        return await CarHTTP.post("/trim", body: body)?.status == 200
    }
}
