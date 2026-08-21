import Foundation
import Combine

/// The one liveness truth, composed from the path, the session and the age of the newest
/// telemetry frame.
///
/// It replaces four independent notions of "connected" that could disagree: a published
/// connection state nothing read, a telemetry-freshness flag, a one-shot `fw != nil` latch, and
/// the launch gate's own phase. When they disagreed the app dropped an opaque overlay over the
/// drive screen that swallowed every touch, and there was no single place to look to find out why.
@MainActor
final class CarLink: ObservableObject {
    struct Radio: Equatable { let fw: String; let ok: Bool }

    @Published private(set) var state: Link = .searching
    /// The car's identity, from the hello reply — this is what the version gate compares.
    @Published private(set) var fw: String?
    @Published private(set) var device: String?
    /// The radio co-processor's firmware, from `/status`. Not carried by telemetry and not part
    /// of the app image, so a pinned-version mismatch is invisible unless it is surfaced.
    @Published private(set) var radio: Radio?
    /// The newest numbers we ever saw, live or not. `state` is the truth about the link; this is
    /// for screens that legitimately show the last known reading (uptime, firmware, trips).
    @Published private(set) var lastTelemetry: Telemetry?

    let path = CarPath()
    private let transport: CarTransport
    private var session: SessionState = .none
    private var telemetry: Telemetry?
    private var lastFrame: ContinuousClock.Instant?
    private var pump: Task<Void, Never>?
    private var decay: Task<Void, Never>?
    private var pathSub: AnyCancellable?
    private var pathState: PathState = .noWifi(.notAvailable)
    #if DEBUG
    /// Set by the debug gallery: hold the seeded state, with no transport and no path behind it.
    private var frozen = false
    #endif

    init(transport: CarTransport = .shared) {
        self.transport = transport
        pathSub = path.$state.sink { [weak self] p in
            guard let self else { return }
            self.pathState = p
            self.recompute()
        }
    }

    var isLive: Bool { state.isLive }

    /// Open the channel. Idempotent; the transport owns the reconnect loop.
    func start() {
        guard pump == nil else { return }
        pump = Task { [weak self] in await self?.consume() }
        // Liveness has to expire on its own: telemetry stopping is silence, and silence
        // generates no event to react to.
        decay = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(200))
                self?.recompute()
            }
        }
        Task { [transport] in await transport.start() }
    }

    /// Leaving the app is `graceful: true` — the car is told to stop rather than left to notice.
    func stop(graceful: Bool) async {
        pump?.cancel(); pump = nil
        decay?.cancel(); decay = nil
        await transport.stop(graceful: graceful)
        telemetry = nil
        lastFrame = nil
        if case .foreign = session {} else { session = .none }
        recompute()
    }

    /// The wrong-car screen's retry: forget the foreign identity and look again. Nothing else
    /// clears it, on purpose — a wrong car is not a transient failure to retry silently.
    func retryAfterWrongCar() {
        session = .none
        device = nil
        recompute()
    }

    private func consume() async {
        for await event in await transport.events() {
            switch event {
            case .sessionOpened(let device, let fw):
                self.device = device
                self.fw = fw
                if device == CarContract.device {
                    session = .adopted(device: device, fw: fw)
                    fetchRadio()
                } else {
                    session = .foreign(device: device)
                }
            case .telemetry(let t):
                telemetry = t
                lastTelemetry = t
                lastFrame = ContinuousClock.now
            case .sessionClosed:
                // A foreign identity survives the session that discovered it: the transport
                // reopens every few seconds and would otherwise flicker the wrong-car screen
                // back to a radar the user has no reason to watch.
                if case .foreign = session {} else { session = .none }
                telemetry = nil
                lastFrame = nil
            }
            recompute()
        }
    }

    private func recompute() {
        #if DEBUG
        if frozen { return }
        #endif
        let age = lastFrame.map { (ContinuousClock.now - $0).seconds }
        state = LinkRule.compose(path: pathState, session: session, telemetry: telemetry, age: age)
    }

    private func fetchRadio() {
        Task { [weak self, transport] in
            guard let data = try? await transport.get("/status", timeout: 2),
                  let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let r = j["radio"] as? [String: Any],
                  let fw = r["fw"] as? String else { return }
            self?.radio = Radio(fw: fw, ok: r["ok"] as? Bool ?? true)
        }
    }

    #if DEBUG
    /// One screen's worth of link, for the gallery. Nothing runs behind it.
    static func preview(_ state: Link, fw: String? = "v1.0+517", radio: Radio? = nil) -> CarLink {
        let l = CarLink()
        l.frozen = true
        l.state = state
        l.fw = fw
        l.device = CarContract.device
        l.radio = radio
        l.lastTelemetry = state.telemetry
        return l
    }
    #endif
}

private extension Duration {
    var seconds: TimeInterval {
        Double(components.seconds) + Double(components.attoseconds) / 1e18
    }
}
