import Foundation

/// REST client for the car's calibration endpoints.
///
/// Runs over `CarHTTP` — a connection bound to the Wi-Fi interface — because `URLSession` stops
/// reaching the car once iOS decides its network has no internet. See `CarNet`.
@MainActor
final class CalibClient {
    func fetchCalibrated() async -> Bool {
        guard let r = await CarHTTP.get("/calib"), r.status == 200,
              let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any] else {
            return false
        }
        return (j["calibrated"] as? Bool) ?? false
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
        await CarHTTP.post(path, body: Data(body.utf8))?.status == 200
    }
}
