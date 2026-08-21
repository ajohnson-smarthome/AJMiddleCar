import Foundation

/// Reads/writes the car's link-loss auto-return config via GET/POST /recover.
struct RecoverClient {
    func get() async -> (enabled: Bool, windowMs: Int)? {
        guard let r = await CarHTTP.get("/recover"), r.status == 200,
              let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any],
              let enabled = j["enabled"] as? Bool,
              let win = j["window_ms"] as? Int else { return nil }
        return (enabled, win)
    }

    @discardableResult
    func set(enabled: Bool, windowMs: Int) async -> Bool {
        struct Body: Encodable { let enabled: Bool; let window_ms: Int }
        guard let body = try? JSONEncoder().encode(Body(enabled: enabled, window_ms: windowMs)) else {
            return false
        }
        return await CarHTTP.post("/recover", body: body)?.status == 200
    }
}
