import Foundation
import Network

/// The control link: `/ws`, ten command frames a second up, telemetry down.
///
/// Runs on `Network.framework` rather than `URLSessionWebSocketTask` for one reason — the
/// connection has to be bound to the Wi-Fi interface, and `URLSession` cannot express that. See
/// `CarNet` for why that matters; the short version is that iOS stops routing general traffic over
/// a network with no internet, and the car is exactly such a network.
@MainActor
final class CarConnection: ObservableObject {
    enum State { case connecting, connected, offline }
    @Published private(set) var state: State = .connecting

    /// Called on the main actor for each telemetry frame pushed by the car.
    var onTelemetry: ((Telemetry) -> Void)?

    private let queue = DispatchQueue(label: "carws")
    private var conn: NWConnection?
    private var timer: DispatchSourceTimer?
    private let outbox = Outbox()
    private var started = false

    /// Latest driving intent; streamed at 10 Hz while connected.
    func setCommand(_ s: String) { outbox.set(s) }

    /// Zero the streamed command and stop the 10 Hz timer — called when the app leaves the
    /// foreground so a backgrounded app can't keep the car driving or burn battery.
    func pause() {
        outbox.set(ControlModel.frame(t: 0, y: 0))
        timer?.cancel(); timer = nil
    }

    /// Re-arm the stream timer when the app returns to the foreground.
    func resume() {
        guard started, timer == nil else { return }
        armTimer()
    }

    func start() {
        /* Idempotent per concern, not per call. `started` guards the socket; the timer
           has its own guard, because pause() cancels the timer and deliberately leaves
           `started` true. Without this, visiting the wrong-car screen — which pauses —
           and coming back left the 10 Hz timer dead for the rest of the process: the
           socket connected, telemetry arrived, the pill said connected, and the
           joysticks did nothing at all. */
        if !started {
            started = true
            connect()
        }
        if timer == nil { armTimer() }
    }

    /// The stream runs on its own dispatch timer, not a `Timer` on the main run loop.
    ///
    /// That is not a style preference. `Timer.scheduledTimer` installs into the run loop's
    /// *default* mode, and while a finger is dragging the joystick the run loop is in tracking
    /// mode — so it stops firing exactly when the car is being driven. The car then sees no
    /// control frame for 300 ms, the watchdog declares the link lost, and `recovery` starts
    /// replaying the path in reverse: wheels turning backwards while the user holds forward.
    private func armTimer() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.1, repeating: 0.1, leeway: .milliseconds(10))
        t.setEventHandler { [weak self] in self?.tick() }
        timer = t
        t.resume()
    }

    deinit { timer?.cancel() }

    // MARK: - connection

    private func connect() {
        guard started else { return }
        state = .connecting
        let c = NWConnection(to: CarNet.wsEndpoint(), using: CarNet.params(webSocket: true))
        conn = c
        c.stateUpdateHandler = { [weak self] st in
            Task { @MainActor in
                guard let self, self.conn === c else { return }
                switch st {
                case .ready:
                    self.state = .connected
                    self.outbox.setConnection(c)
                    self.receive(on: c)
                case .failed, .cancelled:
                    self.retry(after: c)
                case .waiting:
                    // With the interface pinned, waiting means the Wi-Fi path is not there yet.
                    // Tear down and come back in a second rather than sit in a state that may
                    // never resolve.
                    self.retry(after: c)
                default:
                    break
                }
            }
        }
        c.start(queue: queue)
    }

    /// Reads telemetry and, crucially, asks for the next message **before** touching the UI.
    ///
    /// The obvious shape — hop to the main actor, handle the frame, then re-arm — makes the read
    /// loop wait on whatever SwiftUI is doing. Driving is when SwiftUI is busiest: a joystick
    /// gesture, the animated diagram, the power bars. Telemetry then stops being collected exactly
    /// while the car is being driven, `lastFrame` goes stale, and after a second the app declares
    /// the car offline and drops the "searching" screen over the controls. Re-arming first keeps
    /// liveness independent of frame rate.
    private nonisolated func receive(on c: NWConnection) {
        c.receiveMessage { [weak self] data, _, _, error in
            guard let self else { return }
            if error != nil {
                Task { @MainActor in self.retry(after: c) }
                return
            }
            let tele = data
                .flatMap { String(data: $0, encoding: .utf8) }
                .flatMap { Telemetry.parse($0) }

            self.receive(on: c)          // keep reading no matter how busy the interface is

            if let tele {
                Task { @MainActor in
                    guard self.outbox.connection() === c else { return }
                    self.onTelemetry?(tele)
                }
            }
        }
    }

    /// Runs on `queue`, deliberately never on the main actor: hopping to the UI to send a control
    /// frame would put the stream back at the mercy of whatever the interface is doing.
    private nonisolated func tick() {
        guard let c = liveConnection() else { return }
        let meta = NWProtocolWebSocket.Metadata(opcode: .text)
        let ctx = NWConnection.ContentContext(identifier: "cmd", metadata: [meta])
        c.send(content: Data(outbox.get().utf8), contentContext: ctx, isComplete: true,
               completion: .contentProcessed { [weak self] err in
                   guard err != nil else { return }
                   Task { @MainActor in self?.retry(after: c) }
               })
    }

    /// The connection as seen from the timer queue; `nil` until it is ready.
    private nonisolated func liveConnection() -> NWConnection? { outbox.connection() }

    /// Drop this connection and open a new one shortly. Guarded on identity so the several
    /// callbacks that can fail at once only cause one reconnect.
    private func retry(after c: NWConnection) {
        guard conn === c else { return }
        c.cancel()
        conn = nil
        outbox.setConnection(nil)
        state = .offline
        guard started else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            Task { @MainActor in
                guard let self, self.started, self.conn == nil else { return }
                self.connect()
            }
        }
    }
}


/// The command and the live connection, reachable from the send timer without touching the main
/// actor. Small enough that a lock is the honest tool.
private final class Outbox: @unchecked Sendable {
    private let lock = NSLock()
    private var text = ControlModel.frame(t: 0, y: 0)
    private var conn: NWConnection?

    func set(_ s: String) { lock.lock(); text = s; lock.unlock() }
    func get() -> String { lock.lock(); defer { lock.unlock() }; return text }

    func setConnection(_ c: NWConnection?) { lock.lock(); conn = c; lock.unlock() }
    func connection() -> NWConnection? { lock.lock(); defer { lock.unlock() }; return conn }
}
