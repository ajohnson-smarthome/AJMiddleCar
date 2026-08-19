import Foundation

/// Reads/writes the car's link-loss auto-return config via GET/POST /recover.
struct RecoverClient {
    func get() async -> (enabled: Bool, windowMs: Int)? {
        guard let url = URL(string: CarHost.httpBase + "/recover") else { return nil }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let enabled = j["enabled"] as? Bool,
              let win = j["window_ms"] as? Int else { return nil }
        return (enabled, win)
    }

    @discardableResult
    func set(enabled: Bool, windowMs: Int) async -> Bool {
        guard let url = URL(string: CarHost.httpBase + "/recover") else { return false }
        struct Body: Encodable { let enabled: Bool; let window_ms: Int }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.httpBody = try? JSONEncoder().encode(Body(enabled: enabled, window_ms: windowMs))
        guard let (_, resp) = try? await URLSession.shared.data(for: req) else { return false }
        return (resp as? HTTPURLResponse)?.statusCode == 200
    }
}
