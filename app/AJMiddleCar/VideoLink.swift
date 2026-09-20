import Foundation
import Network

/// The video channel on the phone: one UDP socket to the video port, a `view` every
/// `videoSubscribeMs` while someone is watching, reassembly, and the keyframe requests
/// that turn a loss into a flicker instead of a freeze.
///
/// Two conditions gate the subscription and both are the caller's to set: `session(sid:)`
/// from `CarLink` when the car adopts us (the car accepts a view only from that sid), and
/// `setWatching` from the drive screen. Either going away tears the socket down, which
/// stops the views, which stops the stream within `videoSubscribeTimeoutMs` on the car.
///
/// Reassembly runs on `queue`, and `onFrame` is called there: the view enqueues sample
/// buffers straight from it. What crosses to the main actor is the counters, once a second,
/// and `hasPicture` the moment a picture appears — once per appearance, not per frame.
@MainActor
final class VideoLink: ObservableObject {
    @Published private(set) var fps = 0
    @Published private(set) var lostLast10s = 0
    @Published private(set) var hasPicture = false

    /// Called on `queue` with a complete, decodable frame (Annex B) and whether it is an IDR.
    /// It must never block on main: the main actor blocks on `queue` (`queue.sync`) to read
    /// the counters, so a `DispatchQueue.main.sync` inside it is a deadlock.
    ///
    /// Stored inside `State` so the read on `queue` and the write from the main actor cannot
    /// race — a closure store is not atomic against a concurrent load. Never call the getter
    /// from `queue`: it hops there synchronously.
    nonisolated var onFrame: (@Sendable (Data, Bool) -> Void)? {
        get { queue.sync { state.onFrame } }
        set { queue.async { [state] in state.onFrame = newValue } }
    }

    nonisolated let queue = DispatchQueue(label: "ajmiddlecar.video", qos: .userInteractive)

    private var sid: String?
    private var watching = false
    /// When the socket last spoke — the car's subscription timeout counts from here.
    private var lastViewAt: TimeInterval = 0
    /// The car accepted a new bitrate for a running stream: the next open waits until the
    /// car's subscription has lapsed, so the stream restarts and reads it (`VideoReopenHold`).
    private var hold = VideoReopenHold()
    private var conn: NWConnection?
    private var viewTimer: Timer?
    private var statsTimer: Timer?
    private nonisolated let state = State()

    /// Everything the receive path touches, confined to `queue`.
    private final class State: @unchecked Sendable {
        /// Ten one-second intervals need eleven samples: the loss count is `last - first`.
        static let lossSamples = 11

        var onFrame: (@Sendable (Data, Bool) -> Void)?
        var receiver = VideoReceiver()
        /// True from the start: a fresh receiver waits for a keyframe (rule 3), so the first
        /// `view` of a (re)opened socket asks for one. Without the ask, a return to the drive
        /// screen inside `videoSubscribeTimeoutMs` — the settings sheet dismissed, the app
        /// foregrounded — finds the car's subscription still alive: the view is a refresh of
        /// the same stream, no IDR is forced, and the picture waits up to `videoKeyframeS`
        /// for the scheduled one.
        var needKey = true
        var frames = 0
        var lostWindow: [Int] = Array(repeating: 0, count: lossSamples)
        /// Whether there is a picture, and the frame that made one (`PicturePresence`).
        var presence = PicturePresence()
        var lastKeyAskAt: Date = .distantPast

        /// Back to the state a fresh socket deserves — every field, not just the receiver. A
        /// new receiver counts `dropped` from zero, and a window still holding the previous
        /// session's cumulative values would read it as negative losses until it drained.
        /// `needKey` goes back to true for the reason it starts there.
        func reset() {
            receiver = VideoReceiver()
            needKey = true
            frames = 0
            lostWindow = Array(repeating: 0, count: Self.lossSamples)
            presence.reset()
            lastKeyAskAt = .distantPast
        }
    }

    func session(sid: String) {
        self.sid = sid
        reconcile()
    }

    func sessionClosed() {
        sid = nil
        reconcile()
    }

    func setWatching(_ on: Bool) {
        watching = on
        reconcile()
    }

    /// Ask for a keyframe now — the renderer lost its decoder (background) or the receiver
    /// saw a loss. Rate-limited on the car; here just sent.
    func requestKeyframe() {
        queue.async { [state] in state.needKey = true }
        sendView(key: true)
    }

    /// The car accepted a `video` write that changes the bitrate of a stream it may still be
    /// running. It reads the bitrate at stream start only (`car/video-stream`), and a `view`
    /// inside `videoSubscribeTimeoutMs` of the last one refreshes the old stream rather than
    /// starting a new one — so the settings sheet's slider, closed within three seconds, used to
    /// change nothing the driver could see (AJM-122). Let the subscription lapse: close a socket
    /// that is open, and hold the next open until the timeout after the last view has passed.
    func streamRestartNeeded() {
        hold.arm(lastViewAt: lastViewAt)
        if conn != nil { close() }
        reconcile()
    }

    private func reconcile() {
        let want = sid != nil && watching
        if want, conn == nil {
            if let wait = hold.delay(now: Date().timeIntervalSinceReferenceDate) {
                // Come back when the hold is over; `reconcile` is idempotent, so a watcher or a
                // session that changed in the meantime costs nothing, and several of these
                // pending at once open one socket, not several.
                Task { @MainActor in
                    try? await Task.sleep(for: .seconds(wait))
                    reconcile()
                }
            } else {
                open()
            }
        }
        if !want, conn != nil { close() }
    }

    private func open() {
        let c = NWConnection(to: CarNet.videoEndpoint(), using: CarNet.udpParams())
        conn = c
        queue.async { [state] in state.reset() }
        c.stateUpdateHandler = { [weak self, state] st in
            guard case .ready = st else { return }
            // The first view goes the moment the socket is up — waiting a whole period here
            // is a second of black screen at every session start. The handler runs on `queue`,
            // so the keyframe ask is read here and carried across the hop.
            let key = state.needKey
            Task { @MainActor in self?.sendView(key: key) }
        }
        c.start(queue: queue)
        receiveLoop(c)
        viewTimer = Self.timer(every: Double(CarContract.videoSubscribeMs) / 1000) { [weak self] in
            guard let self else { return }
            let key = self.queue.sync { self.state.needKey }   // repeat the ask until a keyframe lands
            self.sendView(key: key)
        }
        statsTimer = Self.timer(every: 1) { [weak self] in self?.publishStats() }
    }

    /// A repeating timer on the main run loop, in `.common` as well as the default mode
    /// `scheduledTimer` registers it in: a gesture in flight puts the main run loop into
    /// tracking, where a default-mode timer does not fire — and a view that stops mid-drive
    /// is a stream that ends on the car three seconds later.
    private static func timer(every interval: TimeInterval, _ tick: @escaping @MainActor @Sendable () -> Void) -> Timer {
        let t = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { _ in
            Task { @MainActor in tick() }
        }
        RunLoop.main.add(t, forMode: .common)
        return t
    }

    private func close() {
        viewTimer?.invalidate(); viewTimer = nil
        statsTimer?.invalidate(); statsTimer = nil
        conn?.cancel(); conn = nil
        fps = 0; lostLast10s = 0; hasPicture = false
    }

    /// The receive loop ended on an error. If `c` is still the live socket — not one `close`
    /// already cancelled, whose loop ends the same way — tear it down and let `reconcile`
    /// open a fresh one while there is still a session and a watcher. On the view's own
    /// cadence rather than at once: a peer that answers every view with an ICMP refusal
    /// would otherwise make this a reconnect loop at loopback speed. `reconcile` is
    /// idempotent, so a `setWatching` or a new session in the meantime costs nothing.
    private func socketFailed(_ c: NWConnection) {
        guard conn === c else { return }
        close()
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(CarContract.videoSubscribeMs))
            reconcile()
        }
    }

    private func sendView(key: Bool) {
        guard let sid, let conn else { return }
        lastViewAt = Date().timeIntervalSinceReferenceDate
        conn.send(content: RTFrame.view(sid: sid, key: key).data(using: .utf8), completion: .contentProcessed { _ in })
    }

    /// nonisolated: it re-arms itself from the socket's queue, where it also runs the receiver.
    private nonisolated func receiveLoop(_ c: NWConnection) {
        c.receiveMessage { [weak self, state] data, _, _, error in
            guard let self else { return }
            if error != nil {
                // The loop ends here; a socket whose receive failed is not coming back on
                // its own, and leaving `conn` and the view timer alive would keep sending
                // views into it. The main actor decides — this is also how our own cancel
                // surfaces, and only it can tell the two apart.
                Task { @MainActor in self.socketFailed(c) }
                return
            }
            if let data {
                switch state.receiver.feed(data) {
                case .frame(let frame, let key):
                    state.frames += 1
                    if state.presence.frame(at: Date()) {
                        // The picture appeared: the placard leaves with this frame, not with
                        // the stats tick up to a second later (AJM-175). Once per appearance,
                        // and only for the live socket — a frame of one `close` already
                        // cancelled, still queued behind the cancel, must not revive the flag
                        // `close` just cleared; the same test `socketFailed` makes.
                        Task { @MainActor in
                            guard self.conn === c else { return }
                            self.hasPicture = true
                        }
                    }
                    if key { state.needKey = false }
                    state.onFrame?(frame, key)
                case .loss:
                    // Once per loss, then the periodic view keeps asking until a keyframe
                    // lands — re-asked no sooner than the car would honour it, so a forced
                    // keyframe lost on the way costs one `idr_min_ms`, not a whole period.
                    let reask = Double(CarContract.videoIdrMinMs) / 1000
                    if !state.needKey || Date().timeIntervalSince(state.lastKeyAskAt) > reask {
                        state.needKey = true
                        state.lastKeyAskAt = Date()
                        Task { @MainActor in self.sendView(key: true) }
                    }
                case .none, .bad:
                    break
                }
            }
            self.receiveLoop(c)
        }
    }

    private func publishStats() {
        let (f, lost, fresh) = queue.sync { () -> (Int, Int, Bool) in
            let f = state.frames
            state.frames = 0
            state.lostWindow.removeFirst()
            state.lostWindow.append(state.receiver.dropped)
            let lost = state.lostWindow.last! - state.lostWindow.first!
            return (f, lost, state.presence.present(at: Date()))
        }
        fps = f
        lostLast10s = max(0, lost)
        // The reverse transition — `staleAfter` without a frame — is decided here, once a
        // second, as it always was; the forward one is raised by the receiver and only
        // confirmed by this write.
        hasPicture = fresh
    }
}
