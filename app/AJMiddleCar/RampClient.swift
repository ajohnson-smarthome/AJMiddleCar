import Foundation

/// Reads/writes the car's acceleration-ramp time (ms) via GET/POST /ramp.
struct RampClient {
    func get() async -> Int? {
        guard let url = URL(string: CarHost.httpBase + "/ramp") else { return nil }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let v = j["ramp_ms"] as? Int else { return nil }
        return v
    }
    @discardableResult
    func set(_ ms: Int) async -> Bool {
        guard let url = URL(string: CarHost.httpBase + "/ramp") else { return false }
        struct Body: Encodable { let ramp_ms: Int }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.httpBody = try? JSONEncoder().encode(Body(ramp_ms: ms))
        guard let (_, resp) = try? await URLSession.shared.data(for: req) else { return false }
        return (resp as? HTTPURLResponse)?.statusCode == 200
    }
}
