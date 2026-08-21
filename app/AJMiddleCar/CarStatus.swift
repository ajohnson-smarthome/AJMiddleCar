import Foundation

@MainActor
final class CarStatus: ObservableObject {
    /// The device identifier this app is willing to drive. Both cars are a softAP serving the
    /// same API at 192.168.4.1, so a different SSID is necessary but not sufficient — join the
    /// wrong network and the app finds a car exactly where it expects one. Without this check
    /// it would drive the other car with the wrong calibration, dimensions and tricks.
    static let expectedDevice = "ajmiddlecar"
    static let expectedSSID   = "AJMiddleCar"

    @Published var online = false
    /// Non-nil when a board answered /status but reported someone else's identifier.
    /// A wrong car is not an offline car and must not be presented as one.
    @Published var foreignDevice: String?
    @Published var uptimeS: Int?
    @Published var calibrated: Bool?
    @Published var fw: String?
    @Published var rssi: Int?
    @Published var wdtTrips: Int?
    @Published var wsFps: Int?
    /// Co-processor (radio) firmware, from /status. Not part of the app image and not carried by
    /// OTA — it is wire-flashed once and pinned — so a mismatch is invisible unless surfaced.
    @Published var radioFw: String?
    @Published var radioOK: Bool?

    private var freshTimer: Timer?
    private var probeTimer: Timer?
    private var lastFrame = Date.distantPast
    private let staleAfter: TimeInterval = 1.0

    /// Bootstrap probe (identity + fw + initial calibrated); then liveness comes from WS.
    ///
    /// The probe repeats until it lands. It used to run exactly once, which quietly stranded the
    /// app: the connect gate advances on `fw`, and only this probe ever sets it — so a single
    /// timed-out request (the phone still joining the car's Wi-Fi, the car still booting) left
    /// the app on "searching" forever, even with telemetry already arriving over the WS.
    func start() {
        bootstrap()
        if probeTimer == nil {
            probeTimer = Self.commonModeTimer(every: 1.5) { [weak self] in
                Task { @MainActor in
                    guard let self else { return }
                    if self.fw == nil && self.foreignDevice == nil { self.bootstrap() } else { self.stopProbe() }
                }
            }
        }
        guard freshTimer == nil else { return }
        freshTimer = Self.commonModeTimer(every: 0.5) { [weak self] in
            Task { @MainActor in
                guard let self else { return }
                if self.online && Date().timeIntervalSince(self.lastFrame) > self.staleAfter {
                    self.online = false
                }
            }
        }
    }

    /// Scheduled in `.common` modes on purpose. A plain `Timer.scheduledTimer` lands in the run
    /// loop's default mode only, so it stops firing while a finger is dragging the joystick —
    /// which is precisely when the car's liveness matters most.
    private static func commonModeTimer(every seconds: TimeInterval,
                                        _ body: @escaping () -> Void) -> Timer {
        let t = Timer(timeInterval: seconds, repeats: true) { _ in body() }
        RunLoop.main.add(t, forMode: .common)
        return t
    }

    func stop() { freshTimer?.invalidate(); freshTimer = nil; stopProbe() }
    private func stopProbe() { probeTimer?.invalidate(); probeTimer = nil }
    deinit { freshTimer?.invalidate(); probeTimer?.invalidate() }

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
        Task { @MainActor in
            guard let r = await CarHTTP.get("/status", timeout: 2), r.status == 200,
                  let j = try? JSONSerialization.jsonObject(with: r.body) as? [String: Any],
                  let dev = j["device"] as? String else { return }

            guard dev == CarStatus.expectedDevice else {
                self.foreignDevice = dev          // reachable, but not our car
                return
            }
            self.foreignDevice = nil
            self.calibrated = j["calibrated"] as? Bool
            self.fw = j["fw"] as? String
            self.uptimeS = j["uptime_s"] as? Int
            if let radio = j["radio"] as? [String: Any] {
                self.radioFw = radio["fw"] as? String
                self.radioOK = radio["ok"] as? Bool
            }
            self.online = true
            self.lastFrame = Date()
        }
    }
}
