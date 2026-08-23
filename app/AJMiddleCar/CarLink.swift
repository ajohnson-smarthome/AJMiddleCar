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
    /// The radio co-processor's firmware, from `/status`. Three states: unknown (still
    /// fetching), known, and unavailable — every `/status` attempt failed. Unavailable is a
    /// state of its own because hiding the line made silence indistinguishable from health
    /// on the one screen that exists to surface a mismatch.
    enum RadioStatus: Equatable {
        case known(fw: String, ok: Bool)
        case unavailable
    }

    @Published private(set) var state: Link = .searching
    /// The car's identity, from the hello reply — this is what the version gate compares.
    @Published private(set) var fw: String?
    @Published private(set) var device: String?
    /// The radio co-processor's firmware, from `/status`. Not carried by telemetry and not part
    /// of the app image, so a pinned-version mismatch is invisible unless it is surfaced.
    @Published private(set) var radio: RadioStatus?
    /// The car's report that the bootloader reverted the last update (decision 1): `/status`
    /// `rollback`, nil until a fresh post-adoption fetch answers (or on firmware that
    /// predates the key). Reset on every adoption so a pre-reboot value cannot leak forward.
    @Published private(set) var rollback: Bool?
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
    private var radioFetch: Task<Void, Never>?
    /// Bumped on every `fetchRadio()` call (including `refreshRadio()`'s). A fetch
    /// whose captured generation no longer matches has been superseded by a newer one —
    /// cancelling `radioFetch` cannot abort an in-flight `transport.get` (the transport
    /// serializes through its own task), so this is the authoritative guard against a
    /// stale response landing after a newer session already asked again.
    private var radioFetchGen = 0
    private var pathSub: AnyCancellable?
    /// Lifecycle operations run strictly in call order. `start()` and `requestStop()` enqueue
    /// synchronously on the main actor, so the order the scene handler calls them in is the
    /// order they execute in — the unstructured stop task racing a later start() is how the
    /// link used to die until the next background/foreground cycle.
    private var lifecycle: Task<Void, Never>?
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

    private func enqueue(_ op: @escaping @MainActor () async -> Void) {
        let prev = lifecycle
        lifecycle = Task { await prev?.value; await op() }
    }

    /// Open the channel. Idempotent; the transport owns the reconnect loop.
    func start() {
        enqueue { [weak self] in await self?.beginPumping() }
    }

    /// Leaving the app is a goodbye said in words — the car is told to stop rather than left
    /// to notice. Synchronous enqueue: callers in view handlers must not need an await for
    /// the ordering guarantee to hold.
    func requestStop(graceful: Bool) {
        enqueue { [weak self] in await self?.shutDown(graceful: graceful) }
    }

    func stop(graceful: Bool) async {
        requestStop(graceful: graceful)
        await lifecycle?.value
    }

    private func beginPumping() async {
        guard pump == nil else { return }
        // Acquire the streams before starting the transport (rather than inside `consume()`,
        // after the pump task is merely spawned): `events()`/`telemetryFrames()` install this
        // session's listeners on the actor by finishing whatever the previous consumer held, and
        // that install must be visible to the actor before `transport.start()` can possibly emit
        // into it — otherwise a `.sessionOpened` racing the still-unscheduled pump task would
        // land in a stream nobody is reading and be lost for good.
        let events = await transport.events()
        let frames = await transport.telemetryFrames()
        pump = Task { [weak self] in await self?.consume(events: events, frames: frames) }
        // Liveness has to expire on its own: telemetry stopping is silence, and silence
        // generates no event to react to.
        decay = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(200))
                self?.recompute()
            }
        }
        await transport.start()
    }

    private func shutDown(graceful: Bool) async {
        // Idempotent: the scene path can enqueue two stops back to back (.inactive, then
        // .background), and the second must be a no-op rather than a second goodbye.
        guard pump != nil else { return }
        pump?.cancel(); pump = nil
        decay?.cancel(); decay = nil
        radioFetch?.cancel(); radioFetch = nil
        // Bounded: a goodbye stuck on a dead path (Wi-Fi gone while the socket was still
        // `.waiting`) must not dam the lifecycle chain forever — every later start/stop queues
        // behind an unbounded await otherwise. 300 ms covers 3 sends at 10 Hz spacing with
        // margin; `transport.stop` is cancel-safe mid-goodbye (`send`'s `onCancel` resolves the
        // pending send, `sayGoodbye`'s `try?` swallows it, and `socket.cancel()` still runs).
        let stop = Task { await transport.stop(graceful: graceful) }
        let deadline = Task { try? await Task.sleep(for: .milliseconds(300)); stop.cancel() }
        await stop.value
        deadline.cancel()
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
        // The transport is mid-hold on the session that discovered the identity; abort it so
        // the next hello goes out now, not after the remainder of the ten seconds.
        Task { [transport] in await transport.retryNow() }
    }

    private func consume(events: AsyncStream<CarTransport.Event>, frames: AsyncStream<Telemetry>) async {
        // Two streams, one consumer. Ordering between them is not guaranteed; a frame from the
        // previous session can land after this session's `.sessionOpened` (or after
        // `.sessionClosed`) — `apply(_:)` gates on an adopted session, so a late frame from the
        // wrong session is simply dropped rather than re-seeding `lastTelemetrySeq` with a stale
        // counter or resurrecting a link that `LinkRule.compose` has already retired.
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
                rollback = nil
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
        // Telemetry only flows inside an adopted session (the transport's `receiveLoop` starts
        // after adoption). Gating here means a frame the previous session's buffer was still
        // holding — and that lands after this session's `.sessionOpened` reset
        // `lastTelemetrySeq` to nil — cannot re-seed it with a stale, free-running counter from
        // the car's old boot and lock every fresh frame out as "not newer" forever.
        guard case .adopted = session else { return }
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

    /// `/status` is one GET against a single-request server that is busy with the geometry
    /// prefetch fired in the same instant — one miss must not hide a radio mismatch for the
    /// whole session. Four tries, backing off; if every try fails the status becomes
    /// `.unavailable` rather than silence. `rollback` is parsed independently of the radio
    /// object: it must survive a malformed radio block, and its absence (older firmware)
    /// stays nil. Every write is guarded by `radioFetchGen`: a fetch superseded by a newer
    /// `.sessionOpened` (e.g. the reconnect right after an OTA reboot) must not let its
    /// in-flight response — which cancellation alone cannot stop — overwrite the newer
    /// session's fresh values.
    private func fetchRadio() {
        radioFetch?.cancel()
        radioFetchGen += 1
        let gen = radioFetchGen
        radioFetch = Task { [weak self, transport] in
            for delay: Double in [0, 1, 2, 4] {
                if delay > 0 { try? await Task.sleep(for: .seconds(delay)) }
                if Task.isCancelled { return }
                guard let self, gen == self.radioFetchGen else { return }
                let data = try? await transport.get("/status", timeout: 2)
                // Authoritative recheck: the await above is exactly where a supersede can land
                // unnoticed by `Task.isCancelled` (the transport's own task keeps running it).
                // `self` is already the non-optional bound above (still in scope, same
                // iteration) — only the generation can have moved.
                guard gen == self.radioFetchGen else { return }
                if let data, let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    if let rb = j["rollback"] as? Bool { self.rollback = rb }
                    if let r = j["radio"] as? [String: Any],
                       let fw = r[CarContract.fwField] as? String {
                        // Missing/malformed `ok` is NOT health (decision 17): unknown means
                        // the one flag this line exists for could not be read.
                        self.radio = .known(fw: fw, ok: r["ok"] as? Bool ?? false)
                        return
                    }
                }
            }
            guard let self, gen == self.radioFetchGen else { return }
            if self.radio == nil { self.radio = .unavailable }
        }
    }

    /// FirmwareView calls this on appear: the radio line is that screen's reason to exist,
    /// and an OTA just behind us may have changed the answer.
    func refreshRadio() { fetchRadio() }

    #if DEBUG
    /// One screen's worth of link, for the gallery. Nothing runs behind it.
    static func preview(_ state: Link, fw: String? = "v1.0+517", radio: RadioStatus? = nil) -> CarLink {
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
