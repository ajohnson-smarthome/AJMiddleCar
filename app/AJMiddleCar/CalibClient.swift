import Foundation

/// REST client for the car's calibration endpoints (uses CarHost's base address,
/// so it talks to the localhost mock in the simulator and 192.168.4.1 on device).
@MainActor
final class CalibClient {
    private var base: String { CarHost.httpBase }

    func fetchCalibrated() async -> Bool {
        guard let url = URL(string: base + "/calib") else { return false }
        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            if let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                return (j["calibrated"] as? Bool) ?? false
            }
        } catch {}
        return false
    }

    func spin(pair: Int, dir: Int) async {
        await post("/calib/spin", body: #"{"pair":\#(pair),"dir":\#(dir)}"#)
    }

    @discardableResult
    func save(body: String) async -> Bool {
        await post("/calib/save", body: body)
    }

    @discardableResult
    private func post(_ path: String, body: String) async -> Bool {
        guard let url = URL(string: base + path) else { return false }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.httpBody = body.data(using: .utf8)
        do {
            let (_, resp) = try await URLSession.shared.data(for: req)
            return (resp as? HTTPURLResponse)?.statusCode == 200
        } catch { return false }
    }
}
