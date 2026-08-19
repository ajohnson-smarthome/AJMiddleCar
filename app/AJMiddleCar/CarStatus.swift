import Foundation

@MainActor
final class CarStatus: ObservableObject {
    @Published var online = false
    @Published var uptimeS: Int?
    @Published var calibrated: Bool?
    @Published var fw: String?
    @Published var rssi: Int?
    @Published var wdtTrips: Int?
    @Published var wsFps: Int?

    private let url = URL(string: CarHost.statusURL)!
    private var freshTimer: Timer?
    private var lastFrame = Date.distantPast
    private let staleAfter: TimeInterval = 1.0

    /// One-shot bootstrap probe (identity + fw + initial calibrated); then liveness comes from WS.
    func start() {
        bootstrap()
        guard freshTimer == nil else { return }
        freshTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                if self.online && Date().timeIntervalSince(self.lastFrame) > self.staleAfter {
                    self.online = false
                }
            }
        }
    }

    func stop() { freshTimer?.invalidate(); freshTimer = nil }
    deinit { freshTimer?.invalidate() }

    /// Apply a telemetry frame pushed over WS.
    func apply(_ t: Telemetry) {
        lastFrame = Date()
        online = true
        rssi = t.rssi
        wsFps = t.wsFps
        wdtTrips = t.wdtTrips
        uptimeS = t.uptimeS
        if let c = t.calibrated { calibrated = c }
    }

    private func bootstrap() {
        var req = URLRequest(url: url)
        req.timeoutInterval = 2
        req.cachePolicy = .reloadIgnoringLocalCacheData
        URLSession.shared.dataTask(with: req) { [weak self] data, _, _ in
            var ok = false; var cal: Bool?; var fwv: String?; var up: Int?
            if let data,
               let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               (j["device"] as? String) == "esp32-car" {
                ok = true; cal = j["calibrated"] as? Bool; fwv = j["fw"] as? String; up = j["uptime_s"] as? Int
            }
            Task { @MainActor in
                guard let self else { return }
                if ok { self.online = true; self.calibrated = cal; self.fw = fwv; self.uptimeS = up; self.lastFrame = Date() }
            }
        }.resume()
    }
}
