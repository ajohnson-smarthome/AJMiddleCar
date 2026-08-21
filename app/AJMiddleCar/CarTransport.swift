import Foundation
import Network

/// The one connection owner: the real-time datagram channel, the REST requests, and the task
/// that keeps them alive.
///
/// What this replaces was four independent variables — `started`, `timer`, `conn`,
/// `outbox.socket` — mutated from six call sites, which is why two reconnect loops could exist at
/// once and why a paused timer could outlive the pause. Here the lifecycle is one task: `run()`
/// loops sessions, a session is a task group, and the first failure takes both loops down
/// together. The old socket is provably dead because `cancelAll()` ends both sides of it and the
/// next session opens with a fresh session id.
actor CarTransport {
    /// Everything that talks to the car goes through this instance. "Strictly one client" is a
    /// property of the protocol; one owner in the app is how it stays true.
    static let shared = CarTransport()

    enum Event: Sendable {
        /// The car answered our hello. `device` may be someone else's — the caller decides.
        case sessionOpened(device: String, fw: String)
        case telemetry(Telemetry)
        case sessionClosed
    }

    /// Hello repeat rate while waiting for adoption. The schema carries no hello rate — it is a
    /// property of this side's impatience, not of the wire — so it lives here.
    private static let helloPeriod = Duration.milliseconds(200)
    /// Nothing received for this long means the session is gone even though the socket still
    /// looks open. UDP offers no other way to notice, and a car that stopped answering has to
    /// become "searching" rather than a live link showing stale numbers.
    private static let stallTimeout: TimeInterval = 3
    private static let backoffBase = 0.1
    private static let backoffCap = 5.0
    private static let queue = DispatchQueue(label: "car.transport")

    // MARK: - the driving command

    private let outbox = CommandBox()

    /// Set from the main actor without an actor hop, so two touches can never be reordered and
    /// the joystick never waits on the transport.
    nonisolated func setCommand(t: Double, y: Double) { outbox.set(t: t, y: y) }

    // MARK: - state

    private var runTask: Task<Void, Never>?
    private var conn: NWConnection?
    private var seq: UInt32 = 0
    private var lastRx = ContinuousClock.now
    private var listener: AsyncStream<Event>.Continuation?
    private var httpTail: [String: Task<Void, Never>] = [:]
    /// Whether the car adopted the session that is running now. A session only ever ends by
    /// failing, so this — not the return of `session()` — is what says the car was reachable.
    private var sessionAdopted = false

    /// The event stream feeding `CarLink`. One consumer by design; a second call replaces it.
    func events() -> AsyncStream<Event> {
        let (stream, cont) = AsyncStream<Event>.makeStream(bufferingPolicy: .bufferingNewest(8))
        listener?.finish()
        listener = cont
        return stream
    }

    private func emit(_ e: Event) { listener?.yield(e) }

    // MARK: - lifecycle

    /// Idempotent. There is one `run()`, so two reconnect loops cannot exist.
    func start() {
        guard runTask == nil else { return }
        runTask = Task { [weak self] in await self?.run() }
    }

    /// Leaving `.active` is this, with `graceful: true` — a goodbye said in words rather than a
    /// cancelled transmitter. The car stops where it stands instead of spending five seconds
    /// retracing its path with the controls off-screen.
    func stop(graceful: Bool) async {
        if graceful, let conn {
            seq = RTFrame.nextSeq(seq)
            // Awaited on purpose: the datagram has to be on the wire before the socket goes.
            try? await send(RTFrame.bye(seq: seq), on: conn)
        }
        runTask?.cancel()
        runTask = nil
        outbox.set(t: 0, y: 0)
        tearDown()
    }

    private func tearDown() {
        conn?.cancel()
        conn = nil
        emit(.sessionClosed)
    }

    private func run() async {
        var attempt = 0
        while !Task.isCancelled {
            do {
                try await session()
            } catch {
                // Any failure ends the session; the difference between them is only in the log.
            }
            let reached = sessionAdopted
            tearDown()
            if Task.isCancelled { break }
            attempt = reached ? 0 : attempt + 1
            try? await Task.sleep(for: backoff(attempt))
        }
    }

    /// Exponential with jitter, capped. The jitter matters when the car reboots under OTA: every
    /// retry from a fixed schedule lands in the same window it did last time.
    private func backoff(_ attempt: Int) -> Duration {
        let raw = min(Self.backoffCap, Self.backoffBase * pow(2, Double(max(0, attempt - 1))))
        return .seconds(raw * Double.random(in: 0.75...1.25))
    }

    /// One session, start to finish. It ends only by failing — which is why reaching the car is
    /// recorded in `sessionAdopted` rather than returned.
    private func session() async throws {
        sessionAdopted = false
        let sid = RTFrame.sessionID()
        let socket = try await connect()
        conn = socket
        lastRx = ContinuousClock.now

        let identity = try await handshake(sid: sid)
        emit(.sessionOpened(device: identity.device, fw: identity.fw))

        guard identity.device == CarContract.device else {
            // Not our car. A single command frame here would drive it, so instead of streaming
            // we say goodbye — the other car drops ownership and stops — and hold the session
            // long enough that the wrong-car screen is not a flicker.
            seq = RTFrame.nextSeq(seq)
            try? await send(RTFrame.bye(seq: seq), on: socket)
            try await Task.sleep(for: .seconds(10))
            throw CarError.malformed("foreign device \(identity.device)")
        }

        sessionAdopted = true
        try await withThrowingTaskGroup(of: Void.self) { group in
            group.addTask { try await self.sendLoop() }
            group.addTask { try await self.receiveLoop() }
            group.addTask { try await self.stallGuard() }
            try await group.next()          // the first failure ends the session
            group.cancelAll()
        }
    }

    // MARK: - session phases

    private struct Identity { let device: String; let fw: String }

    /// Hello at ~5 Hz until the car answers. The reply carries the car's identity, so identity
    /// arrives on the first exchange over the channel that then carries telemetry — rather than
    /// from a separate `/status` probe that cancelled itself after one success.
    private func handshake(sid: String) async throws -> Identity {
        try await withThrowingTaskGroup(of: Identity?.self) { group in
            group.addTask { try await self.helloLoop(sid: sid); return nil }
            group.addTask { try await self.awaitHello(sid: sid) }
            group.addTask { try await self.stallGuard(); return nil }
            while let result = try await group.next() {
                if let identity = result {
                    group.cancelAll()
                    return identity
                }
            }
            throw CarError.timeout(Self.stallTimeout)
        }
    }

    private func helloLoop(sid: String) async throws {
        while !Task.isCancelled {
            guard let conn else { throw CarError.refused }
            try await send(RTFrame.hello(sid: sid), on: conn)
            try await Task.sleep(for: Self.helloPeriod)
        }
    }

    private func awaitHello(sid: String) async throws -> Identity? {
        while !Task.isCancelled {
            guard let text = try await receiveOne() else { continue }
            // A reply for another session id is a leftover from the previous socket; ignoring it
            // is what makes ownership non-resumable rather than accidentally inherited.
            if case .helloReply(let replySid, let device, let fw) = RTFrame.parse(text), replySid == sid {
                return Identity(device: device, fw: fw)
            }
        }
        return nil
    }

    /// Ten a second, paced against a deadline rather than a repeating timer, so a slow send
    /// costs one late frame instead of shifting every frame after it.
    private func sendLoop() async throws {
        let period = Duration.seconds(1) / CarContract.commandHz
        var deadline = ContinuousClock.now
        while !Task.isCancelled {
            guard let conn else { throw CarError.refused }
            let c = outbox.get()
            seq = RTFrame.nextSeq(seq)
            try await send(RTFrame.command(seq: seq, t: c.t, y: c.y), on: conn)
            deadline += period
            // A send that overran the period must not be repaid with a burst of catch-up frames.
            if deadline < ContinuousClock.now { deadline = ContinuousClock.now + period }
            try await Task.sleep(until: deadline, clock: .continuous)
        }
    }

    private func receiveLoop() async throws {
        while !Task.isCancelled {
            guard let text = try await receiveOne() else { continue }
            if case .telemetry(let t) = RTFrame.parse(text) { emit(.telemetry(t)) }
        }
    }

    private func stallGuard() async throws {
        while !Task.isCancelled {
            try await Task.sleep(for: .milliseconds(200))
            if ContinuousClock.now - lastRx > .seconds(Self.stallTimeout) {
                throw CarError.timeout(Self.stallTimeout)
            }
        }
    }

    // MARK: - socket

    private func connect() async throws -> NWConnection {
        let socket = NWConnection(to: CarNet.rtEndpoint(), using: CarNet.udpParams())
        let once = OneShot<Void>()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                once.attach(c)
                socket.stateUpdateHandler = { state in
                    switch state {
                    case .ready:
                        once.resume(.success(()))
                    case .failed(let e):
                        once.resume(.failure(CarError.from(e, path: socket.currentPath)))
                    case .waiting(let e):
                        // With the interface pinned, waiting means the Wi-Fi path is not there
                        // yet. Fail now and let the backoff retry rather than sit in a state
                        // that may never resolve.
                        once.resume(.failure(CarError.from(e, path: socket.currentPath)))
                    case .cancelled:
                        once.resume(.failure(CancellationError()))
                    default:
                        break
                    }
                }
                socket.start(queue: Self.queue)
            }
        } onCancel: {
            once.cancel()
            socket.cancel()
        }
        return socket
    }

    private func send(_ text: String, on socket: NWConnection) async throws {
        let data = Data(text.utf8)
        guard data.count <= CarContract.maxDatagram else {
            throw CarError.malformed("datagram \(data.count) B over the \(CarContract.maxDatagram) B cap")
        }
        let once = OneShot<Void>()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                once.attach(c)
                socket.send(content: data, completion: .contentProcessed { err in
                    if let err { once.resume(.failure(CarError.from(err, path: socket.currentPath))) }
                    else { once.resume(.success(())) }
                })
            }
        } onCancel: {
            once.cancel()
        }
    }

    /// One datagram. nil means "arrived but unreadable" — a stray datagram costs one iteration,
    /// never the session.
    private func receiveOne() async throws -> String? {
        guard let socket = conn else { throw CarError.refused }
        let once = OneShot<String?>()
        let text = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<String?, Error>) in
                once.attach(c)
                socket.receiveMessage { data, _, _, error in
                    if let error {
                        once.resume(.failure(CarError.from(error, path: socket.currentPath)))
                        return
                    }
                    once.resume(.success(data.flatMap { String(data: $0, encoding: .utf8) }))
                }
            }
        } onCancel: {
            once.cancel()
        }
        lastRx = ContinuousClock.now       // anything at all from the car counts as alive
        return text
    }

    // MARK: - REST

    /// Absorbed from `CarHTTP`: the same smallest-possible HTTP/1.1 client, now returning a
    /// reason when it fails and serialised per path, because the car serves one request at a
    /// time and a settings screen that fires two GETs at once gets one of them refused.
    func get(_ path: String, timeout: TimeInterval = 3) async throws -> Data {
        try await request("GET", path, body: nil, contentType: nil, timeout: timeout, progress: nil)
    }

    @discardableResult
    func post(_ path: String,
              body: Data,
              contentType: String = "application/json",
              timeout: TimeInterval = 5,
              progress: (@Sendable (Double) -> Void)? = nil) async throws -> Data {
        try await request("POST", path, body: body, contentType: contentType,
                          timeout: timeout, progress: progress)
    }

    private func request(_ method: String,
                         _ path: String,
                         body: Data?,
                         contentType: String?,
                         timeout: TimeInterval,
                         progress: (@Sendable (Double) -> Void)?) async throws -> Data {
        let predecessor = httpTail[path]
        let mine = Task<Data, Error> {
            await predecessor?.value                  // wait for the previous call on this path
            let r = try await HTTPRequest.perform(method: method, path: path, body: body,
                                                  contentType: contentType, timeout: timeout,
                                                  progress: progress)
            guard r.status == 200 else { throw CarError.http(status: r.status, body: r.body) }
            return r.body
        }
        httpTail[path] = Task { _ = try? await mine.value }
        return try await mine.value
    }
}

/// The latest driving intent, readable from the send loop and writable from the main actor with
/// no hop between them. Small enough that a lock is the honest tool.
private final class CommandBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: (t: Double, y: Double) = (0, 0)

    func set(t: Double, y: Double) { lock.lock(); value = (t, y); lock.unlock() }
    func get() -> (t: Double, y: Double) { lock.lock(); defer { lock.unlock() }; return value }
}

/// Resumes a continuation exactly once, and survives being cancelled before it is attached.
/// `NWConnection` reports failure from several callbacks that can fire together, and a
/// continuation resumed twice is a crash rather than a bug report.
private final class OneShot<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var cont: CheckedContinuation<T, Error>?
    private var pending: Result<T, Error>?
    private var done = false

    func attach(_ c: CheckedContinuation<T, Error>) {
        lock.lock()
        if let pending {
            done = true
            lock.unlock()
            c.resume(with: pending)
            return
        }
        cont = c
        lock.unlock()
    }

    func resume(_ result: Result<T, Error>) {
        lock.lock()
        guard !done else { lock.unlock(); return }
        if let c = cont {
            done = true
            cont = nil
            lock.unlock()
            c.resume(with: result)
            return
        }
        pending = result
        lock.unlock()
    }

    func cancel() { resume(.failure(CancellationError())) }
}

/// One HTTP request, start to finish. All state is touched only on `queue`, and `finish` is
/// idempotent so the deadline and the connection callbacks can race without resuming twice.
private final class HTTPRequest: @unchecked Sendable {
    static func perform(method: String,
                        path: String,
                        body: Data?,
                        contentType: String?,
                        timeout: TimeInterval,
                        progress: (@Sendable (Double) -> Void)?) async throws -> (status: Int, body: Data) {
        try await withCheckedThrowingContinuation { cont in
            HTTPRequest(method: method, path: path, body: body, contentType: contentType,
                        timeout: timeout, progress: progress) { cont.resume(with: $0) }
                .start()
        }
    }

    private let method: String
    private let path: String
    private let body: Data?
    private let contentType: String?
    private let timeout: TimeInterval
    private let progress: (@Sendable (Double) -> Void)?
    private let done: (Result<(status: Int, body: Data), Error>) -> Void

    private let queue = DispatchQueue(label: "car.http")
    private var conn: NWConnection?
    private var buf: [UInt8] = []
    private var finished = false
    private var selfRetain: HTTPRequest?

    private init(method: String, path: String, body: Data?, contentType: String?,
                 timeout: TimeInterval, progress: (@Sendable (Double) -> Void)?,
                 done: @escaping (Result<(status: Int, body: Data), Error>) -> Void) {
        self.method = method
        self.path = path
        self.body = body
        self.contentType = contentType
        self.timeout = timeout
        self.progress = progress
        self.done = done
    }

    private func start() {
        selfRetain = self                    // stay alive until finish(); nobody else holds us
        let c = NWConnection(to: CarNet.endpoint(), using: CarNet.tcpParams())
        conn = c
        c.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.write()
            case .failed(let e):
                self.finish(.failure(CarError.from(e, path: c.currentPath)))
            case .cancelled:
                self.finish(.failure(CancellationError()))
            case .waiting:
                // A path that never becomes available is exactly the failure this class exists
                // to survive, so waiting is left to the deadline rather than treated as progress.
                break
            default:
                break
            }
        }
        queue.asyncAfter(deadline: .now() + timeout) { [weak self] in
            guard let self else { return }
            self.finish(.failure(CarError.timeout(self.timeout)))
        }
        c.start(queue: queue)
    }

    private func headBytes() -> Data {
        let hostHeader = CarHost.port == 80 ? CarHost.host : "\(CarHost.host):\(CarHost.port)"
        var head = "\(method) \(path) HTTP/1.1\r\n"
        head += "Host: \(hostHeader)\r\n"
        head += "Connection: close\r\n"
        if let body {
            head += "Content-Type: \(contentType ?? "application/octet-stream")\r\n"
            head += "Content-Length: \(body.count)\r\n"
        }
        head += "\r\n"
        return Data(head.utf8)
    }

    private func write() {
        conn?.send(content: headBytes(), completion: .contentProcessed { [weak self] err in
            guard let self else { return }
            if let err {
                self.finish(.failure(CarError.from(err, path: self.conn?.currentPath)))
                return
            }
            self.writeBody(from: 0)
        })
    }

    /// The body goes out in chunks so the firmware upload can report progress; for the small
    /// JSON bodies everything else sends, this is a single pass.
    private func writeBody(from offset: Int) {
        guard let body, offset < body.count else { read(); return }
        let end = min(offset + 32 * 1024, body.count)
        conn?.send(content: body.subdata(in: offset..<end), completion: .contentProcessed {
            [weak self] err in
            guard let self else { return }
            if let err {
                self.finish(.failure(CarError.from(err, path: self.conn?.currentPath)))
                return
            }
            self.progress?(Double(end) / Double(body.count))
            self.writeBody(from: end)
        })
    }

    private func read() {
        conn?.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) {
            [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty { self.buf.append(contentsOf: data) }

            if let head = HTTPParse.head(self.buf) {
                let available = self.buf.count - head.bodyOffset
                if let want = head.contentLength, available >= want {
                    self.finish(.success((head.status, self.slice(head.bodyOffset, want))))
                    return
                }
                if isComplete || error != nil {
                    if let want = head.contentLength, available < want {
                        self.finish(.failure(CarError.truncated(got: available, want: want)))
                    } else {
                        self.finish(.success((head.status, self.slice(head.bodyOffset, available))))
                    }
                    return
                }
            } else if isComplete || error != nil {
                self.finish(.failure(CarError.malformed("no parseable response head")))
                return
            }
            self.read()
        }
    }

    private func slice(_ offset: Int, _ count: Int) -> Data {
        guard count > 0, offset + count <= buf.count else { return Data() }
        return Data(buf[offset..<(offset + count)])
    }

    private func finish(_ result: Result<(status: Int, body: Data), Error>) {
        guard !finished else { return }
        finished = true
        conn?.cancel()
        conn = nil
        done(result)
        selfRetain = nil
    }
}
