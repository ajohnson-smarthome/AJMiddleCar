import Foundation

/// Reads/writes the car's straight-line trim (pct, -30..30) via GET/POST /trim.
struct TrimClient {
    func get() async -> Int? {
        guard let url = URL(string: CarHost.httpBase + "/trim") else { return nil }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let v = j["trim_pct"] as? Int else { return nil }
        return v
    }
    @discardableResult
    func set(_ pct: Int) async -> Bool {
        guard let url = URL(string: CarHost.httpBase + "/trim") else { return false }
        struct Body: Encodable { let trim_pct: Int }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.httpBody = try? JSONEncoder().encode(Body(trim_pct: pct))
        guard let (_, resp) = try? await URLSession.shared.data(for: req) else { return false }
        return (resp as? HTTPURLResponse)?.statusCode == 200
    }
}
