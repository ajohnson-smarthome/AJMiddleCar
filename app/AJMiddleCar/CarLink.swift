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

    /// Optional so the debug gallery can hold a frozen link without two real `NWPathMonitor`s
    /// running behind every frame it builds.
    private let path: CarPath?
    private let transport: CarTransport
    private let config: ConfigStore?
    private var session: SessionState = .none
    private var telemetry: Telemetry?
    /// The newest telemetry counter accepted, for ordering. The car increments it per push.
    private var lastTelemetrySeq: Int?
    private var lastFrame: ContinuousClock.Instant?
    private var pump: Task<Void, Never>?
    private var decay: Task<Void, Never>?
    private var pathSub: AnyCancellable?
    private var pathState: PathState = .noWifi(.notAvailable)
    #if DEBUG
    /// Set by the debug gallery: hold the seeded state, with no transport and no path behind it.
    private var frozen = false
    #endif

    init(transport: CarTransport = .shared, monitorsPath: Bool = true, config: ConfigStore? = nil) {
        self.transport = transport
        self.config = config ?? (monitorsPath ? .shared : nil)
        guard monitorsPath else { path = nil; return }
        let monitor = CarPath()
        path = monitor
        pathSub = monitor.$state.sink { [weak self] p in
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
        lastTelemetrySeq = nil
        // Backgrounding is not a verdict on who the car is. Leaving `.active` — which a Control
        // Center pull-down alone does — used to clear `.protoMismatch`, so the screen naming the
        // mismatch flipped to the radar until the next hello reply landed.
        session = session.survivingSessionEnd
        recompute()
    }

    /// The wrong-car and wrong-protocol screens' retry: forget what the car said about itself and
    /// look again. Nothing else clears either, on purpose — neither is a transient failure to
    /// retry silently behind a radar sweep.
    func retryAfterWrongCar() {
        session = .none
        device = nil
        recompute()
    }

    private func consume() async {
        let events = await transport.events()
        let frames = await transport.telemetryFrames()
        // Two streams, one consumer. Ordering between them is not guaranteed; a stale frame
        // landing after `.sessionClosed` only refreshes `lastTelemetry` — `LinkRule.compose`
        // still requires an adopted session to say `.live`, so it cannot resurrect the link.
        await withTaskGroup(of: Void.self) { group in
            group.addTask { @MainActor [weak self] in
                for await e in events {
                    self?.handle(e)
                    self?.recompute()
                }
            }
            group.addTask { @MainActor [weak self] in
                for await t in frames {
                    self?.apply(t)
                    self?.recompute()
                }
            }
            await group.waitForAll()
        }
    }

    private func handle(_ event: CarTransport.Event) {
        switch event {
        case .sessionOpened(let device, let fw):
            self.device = device
            lastTelemetrySeq = nil
            if device == CarContract.device {
                // The firmware version is published only for our own car. It feeds the launch
                // gate, and a foreign car's build number there can force an OTA onto a car
                // that is not ours — routing straight around the wrong-car screen.
                self.fw = fw
                session = .adopted(device: device, fw: fw)
                fetchRadio()
                // The car is reachable exactly now. Prefetching from `onAppear` ran while the
                // gate was still talking to GitHub, so both GETs timed out and every trick
                // spent the session on the fallback geometry the `/dims` work replaced.
                config?.prefetchDriveGeometry()
            } else {
                self.fw = nil
                session = .foreign(device: device)
            }
        case .protoMismatch(let theirs):
            self.fw = nil
            self.device = nil
            session = .protoMismatch(theirs: theirs)
        case .sessionClosed:
            // A foreign identity — or a protocol we cannot speak — survives the session that
            // discovered it: the transport reopens every few seconds and would otherwise
            // flicker the screen naming the problem back to a radar.
            session = session.survivingSessionEnd
            telemetry = nil
            lastFrame = nil
            lastTelemetrySeq = nil
        }
    }

    private func apply(_ t: Telemetry) {
        // Ordered by the car's own counter: a reordered datagram walks uptime, the trip
        // count and the calibration flag backwards, and the mandatory-calibration sheet
        // keys on that flag.
        if let seq = t.seq, let last = lastTelemetrySeq, !RTFrame.seqNewer(seq, than: last) {
            return
        }
        if let seq = t.seq { lastTelemetrySeq = seq }
        telemetry = t
        if lastTelemetry != t { lastTelemetry = t }
        lastFrame = ContinuousClock.now
    }

    private func recompute() {
        #if DEBUG
        if frozen { return }
        #endif
        let age = lastFrame.map { (ContinuousClock.now - $0).seconds }
        let next = LinkRule.compose(path: pathState, session: session, telemetry: telemetry, age: age)
        // Only on a real change: the decay tick re-asks five times a second, and `@Published`
        // emits on assignment whether or not the value moved.
        if state != next { state = next }
    }

    private func fetchRadio() {
        Task { [weak self, transport] in
            guard let data = try? await transport.get("/status", timeout: 2),
                  let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let r = j["radio"] as? [String: Any],
                  let fw = r[CarContract.fwField] as? String else { return }
            self?.radio = Radio(fw: fw, ok: r["ok"] as? Bool ?? true)
        }
    }

    #if DEBUG
    /// One screen's worth of link, for the gallery. Nothing runs behind it.
    static func preview(_ state: Link, fw: String? = "v1.0+517", radio: Radio? = nil) -> CarLink {
        let l = CarLink(monitorsPath: false)
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
