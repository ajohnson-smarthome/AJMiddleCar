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
/// buffers straight from it. Only the counters cross to the main actor, once a second.
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
        var needKey = false
        var frames = 0
        var lostWindow: [Int] = Array(repeating: 0, count: lossSamples)
        var lastFrameAt: Date = .distantPast
        var lastKeyAskAt: Date = .distantPast

        /// Back to the state a fresh socket deserves — every field, not just the receiver. A
        /// new receiver counts `dropped` from zero, and a window still holding the previous
        /// session's cumulative values would read it as negative losses until it drained.
        func reset() {
            receiver = VideoReceiver()
            needKey = false
            frames = 0
            lostWindow = Array(repeating: 0, count: Self.lossSamples)
            lastFrameAt = .distantPast
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

    private func reconcile() {
        let want = sid != nil && watching
        if want, conn == nil { open() }
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
        viewTimer = Timer.scheduledTimer(withTimeInterval: Double(CarContract.videoSubscribeMs) / 1000, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                let key = self.queue.sync { self.state.needKey }   // repeat the ask until a keyframe lands
                self.sendView(key: key)
            }
        }
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.publishStats() }
        }
    }

    private func close() {
        viewTimer?.invalidate(); viewTimer = nil
        statsTimer?.invalidate(); statsTimer = nil
        conn?.cancel(); conn = nil
        fps = 0; lostLast10s = 0; hasPicture = false
    }

    private func sendView(key: Bool) {
        guard let sid, let conn else { return }
        conn.send(content: RTFrame.view(sid: sid, key: key).data(using: .utf8), completion: .contentProcessed { _ in })
    }

    /// nonisolated: it re-arms itself from the socket's queue, where it also runs the receiver.
    private nonisolated func receiveLoop(_ c: NWConnection) {
        c.receiveMessage { [weak self, state] data, _, _, error in
            guard let self, error == nil else { return }
            if let data {
                switch state.receiver.feed(data) {
                case .frame(let frame, let key):
                    state.frames += 1
                    state.lastFrameAt = Date()
                    if key { state.needKey = false }
                    state.onFrame?(frame, key)
                case .loss:
                    // Once per loss, then the periodic view keeps asking until a keyframe lands.
                    if !state.needKey || Date().timeIntervalSince(state.lastKeyAskAt) > 1 {
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
            return (f, lost, Date().timeIntervalSince(state.lastFrameAt) < 2)
        }
        fps = f
        lostLast10s = max(0, lost)
        hasPicture = fresh
    }
}
