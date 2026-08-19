import Foundation

@MainActor
final class CarConnection: ObservableObject {
    enum State { case connecting, connected, offline }
    @Published private(set) var state: State = .connecting

    /// Called on the main actor for each telemetry frame pushed by the car.
    var onTelemetry: ((Telemetry) -> Void)?

    private let url = URL(string: CarHost.wsURL)!
    private var task: URLSessionWebSocketTask?
    private var timer: Timer?
    private var command = ControlModel.frame(t: 0, y: 0)
    private var started = false

    /// Latest driving intent; streamed at 10 Hz while connected.
    func setCommand(_ s: String) { command = s }

    /// Zero the streamed command and stop the 10 Hz timer — called when the app leaves the
    /// foreground so a backgrounded app can't keep the car driving or burn battery.
    func pause() {
        command = ControlModel.frame(t: 0, y: 0)
        timer?.invalidate(); timer = nil
    }

    /// Re-arm the stream timer when the app returns to the foreground.
    func resume() {
        guard started, timer == nil else { return }
        armTimer()
    }

    func start() {
        guard !started else { return }
        started = true
        connect()
        armTimer()
    }

    private func armTimer() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    deinit { timer?.invalidate() }

    private func tick() {
        guard state == .connected, let task else { return }
        task.send(.string(command)) { [weak self] err in
            if err != nil { Task { @MainActor in self?.drop() } }
        }
    }

    private func connect() {
        state = .connecting
        let t = URLSession.shared.webSocketTask(with: url)
        task = t
        t.resume()
        t.sendPing { [weak self] err in
            Task { @MainActor in
                guard let self, self.task === t else { return }
                self.state = (err == nil) ? .connected : .offline
                if err != nil { self.scheduleReconnect() }
            }
        }
        receive(on: t)
    }

    private func receive(on t2: URLSessionWebSocketTask) {
        t2.receive { [weak self] result in
            Task { @MainActor in
                guard let self, self.task === t2 else { return }
                switch result {
                case .success(let message):
                    if case .string(let s) = message, let tele = Telemetry.parse(s) {
                        self.onTelemetry?(tele)
                    }
                    self.receive(on: t2)
                case .failure: self.drop()
                }
            }
        }
    }

    private func drop() {
        guard task != nil else { return }  // already dropped — avoid double reconnect
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
        state = .offline
        scheduleReconnect()
    }

    private func scheduleReconnect() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            Task { @MainActor in
                guard let self, self.state == .offline else { return }
                self.connect()
            }
        }
    }
}
