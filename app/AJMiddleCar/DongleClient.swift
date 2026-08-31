import Foundation
import Network

/// The dongle's own small API: `GET /status`, `GET`/`POST /net`, `POST /ota` on
/// `DongleContract.host` : `.port`. This is unrelated to the car's API even though both live in
/// this app — it is the dongle answering for itself, before or regardless of whatever it is
/// relaying. Modelled on `CalibClient.swift`: the same shape, just a different address and a
/// different set of paths.
///
/// **Why this does not go through `CarTransport`.** `CarTransport`'s REST half is not
/// parameterized by host — its private request type always opens its socket at `CarNet.endpoint()`
/// and writes that one fixed host into the `Host:` header, because "exactly one client, exactly
/// one car" is a property its whole design leans on. Retargeting it at the dongle would mean
/// widening that hardwiring, which is a change to `CarTransport.swift` — outside this task's scope
/// (`Modify: none`) and not this task's file to change. So this file carries its own minimal
/// HTTP/1.1 client instead, built the same way `CarTransport`'s is: one `NWConnection` per request,
/// `Connection: close`, chunked body writes so an upload can report progress. What genuinely is
/// shared, unmodified: `HTTPParse` (splitting a response head is host-agnostic) and `CarError` (the
/// same failure vocabulary the rest of the app already reasons about, so a dongle that never
/// answers reads the same as a car that never answers).
///
/// **Which interface — Ruling 1 in the plan's ledger.** This calls `CarNet.tcpParams()`, the public
/// function, rather than reaching for `requiredInterfaceType` itself, so it pins exactly as every
/// other request to the car does. That indirection earned itself on 2026-08-31: U1 came back from
/// the bench answered "not that way at all" — pinning to a guessed *interface type* could not open
/// a socket to a dongle the phone was demonstrably talking to — and the repair landed entirely in
/// `CarNet` and the new `CarInterface`, with not a line changed here.
///
/// **No credential state.** Neither `join` nor `retryJoin` remembers a password between calls —
/// this file is handed opaque strings by its caller and has no idea whose network they are, let
/// alone anywhere to keep one. `retryJoin` takes the same arguments as `join` for exactly that
/// reason; see its doc comment.
///
/// **No per-path request serialization**, unlike `CarTransport` (whose `httpTail` queues
/// concurrent calls to the same path). Callers should keep this client single-flight — the
/// dongle's `httpd` allows three sockets with LRU purge enabled, so overlapping requests are
/// survivable but not free, and nothing here queues them for you.
@MainActor
final class DongleClient {
    init() {}

    func status() async throws -> DongleStatus {
        try DongleStatus.parse(try await get(DongleContract.statusPath))
    }

    func net() async throws -> DongleNet {
        try DongleNet.parse(try await get(DongleContract.netPath))
    }

    func join(ssid: String, password: String) async throws {
        try await postCredentials(ssid: ssid, password: password)
    }

    /// U2. Today this re-POSTs the same credentials `join()` would send — the plan's claim is
    /// that this is what the dongle's firmware acts on when the station is not connected. It is a
    /// separate entry point rather than a second call to `join()` because that behaviour has never
    /// run against real hardware: if the bench shows `POST /net` is not actually the retry lever,
    /// exactly one function changes here, and `join()` and everyone who calls it is untouched.
    ///
    /// Takes `ssid`/`password` rather than remembering them from a prior `join()` call, because
    /// this client deliberately holds no credential state of its own — the caller (the flow layer,
    /// which already holds `CarContract`) has them at any instant a retry is needed. That includes
    /// a cold app launch that finds the dongle already `configured: true, net.state: failed`: that
    /// state persists on the dongle across relaunches, and it is exactly the case this function
    /// exists to answer — a version that instead remembered credentials only from a `join()` made
    /// earlier in the same process could never reach it. `join` and `retryJoin` end up taking the
    /// same arguments and stay meaningfully distinct anyway, which is honest rather than redundant:
    /// `join` means "configure this network", `retryJoin` means "ask again with what you already
    /// have".
    func retryJoin(ssid: String, password: String) async throws {
        try await postCredentials(ssid: ssid, password: password)
    }

    private func postCredentials(ssid: String, password: String) async throws {
        let body: [String: Any] = [
            DongleContract.ssidField: ssid,
            DongleContract.passwordField: password,
        ]
        let data = try JSONSerialization.data(withJSONObject: body)
        _ = try await post(DongleContract.netPath, body: data)
    }

    /// The upload is the car's shape: a raw image in one request, `application/octet-stream`, no
    /// envelope — "the dongle's `/ota` is the car's shape"
    /// (`docs/superpowers/specs/2026-08-30-dongle-api-design.md`). What differs is only the address
    /// this reaches, which is the entire reason this file exists rather than a second call site on
    /// `UpdateClient` — see the type doc above for why `UpdateClient`'s own upload path
    /// (`CarTransport.post`) cannot be pointed at the dongle without changing `CarTransport.swift`.
    func uploadFirmware(_ data: Data, progress: @escaping (Double) -> Void) async throws {
        _ = try await post(DongleContract.otaPath,
                           body: data,
                           contentType: "application/octet-stream",
                           timeout: Self.otaTimeout) { p in
            Task { @MainActor in progress(p) }
        }
    }

    /// Matches the car's own `/ota` budget (`UpdateClient.upload`): the device abandons a stalled
    /// transfer well inside this window, so anything longer is the phone watching a corpse.
    private static let otaTimeout: TimeInterval = 45

    // MARK: - REST

    private func get(_ path: String, timeout: TimeInterval = 3) async throws -> Data {
        try await request("GET", path, body: nil, contentType: nil, timeout: timeout, progress: nil)
    }

    @discardableResult
    private func post(_ path: String,
                      body: Data,
                      contentType: String = "application/json",
                      timeout: TimeInterval = 5,
                      progress: (@Sendable (Double) -> Void)? = nil) async throws -> Data {
        try await request("POST", path, body: body, contentType: contentType, timeout: timeout,
                          progress: progress)
    }

    private func request(_ method: String,
                         _ path: String,
                         body: Data?,
                         contentType: String?,
                         timeout: TimeInterval,
                         progress: (@Sendable (Double) -> Void)?) async throws -> Data {
        let r = try await DongleHTTPRequest.perform(method: method, path: path, body: body,
                                                    contentType: contentType, timeout: timeout,
                                                    progress: progress)
        guard r.status == 200 else { throw CarError.http(status: r.status, body: r.body) }
        return r.body
    }
}

/// One HTTP/1.1 request to the dongle, start to finish — the same technique as `CarTransport`'s
/// private request type (one `NWConnection` per request, `Connection: close`, `HTTPParse` for the
/// response head, `CarError` for every way it can fail), aimed at `DongleContract.host` instead of
/// the car. All state is touched only on `queue`, and `finish` is idempotent so the timeout and the
/// connection callbacks can race without resuming twice.
///
/// A deliberate twin of `CarTransport.swift`'s private `HTTPRequest` rather than a shared type —
/// extracting a shared, host-parameterized core means editing the car's proven transport in the
/// middle of a cutover that has never run on hardware, and that trade is wrong before the bench,
/// right after it. The price is paid knowingly: **a fix to one belongs in both.**
private final class DongleHTTPRequest: @unchecked Sendable {
    static func perform(method: String,
                        path: String,
                        body: Data?,
                        contentType: String?,
                        timeout: TimeInterval,
                        progress: (@Sendable (Double) -> Void)?) async throws -> (status: Int, body: Data) {
        let req = DongleHTTPRequest(method: method, path: path, body: body, contentType: contentType,
                                    timeout: timeout, progress: progress)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { cont in
                req.attach { cont.resume(with: $0) }
                req.start()
            }
        } onCancel: {
            req.cancelExternally()
        }
    }

    private let method: String
    private let path: String
    private let body: Data?
    private let contentType: String?
    private let timeout: TimeInterval
    private let progress: (@Sendable (Double) -> Void)?
    private var completion: ((Result<(status: Int, body: Data), Error>) -> Void)?
    private var pendingResult: Result<(status: Int, body: Data), Error>?

    private let queue = DispatchQueue(label: "dongle.http")
    private var conn: NWConnection?
    private var buf: [UInt8] = []
    private var finished = false
    private var selfRetain: DongleHTTPRequest?

    private init(method: String, path: String, body: Data?, contentType: String?,
                 timeout: TimeInterval, progress: (@Sendable (Double) -> Void)?) {
        self.method = method
        self.path = path
        self.body = body
        self.contentType = contentType
        self.timeout = timeout
        self.progress = progress
    }

    /// Attach the continuation's resume. If the request already finished (external cancel racing
    /// start), deliver the stored result immediately — same idempotence contract as `finish`.
    func attach(_ c: @escaping (Result<(status: Int, body: Data), Error>) -> Void) {
        queue.async {
            if let r = self.pendingResult {
                self.pendingResult = nil
                c(r)
            } else {
                self.completion = c
            }
        }
    }

    /// External (task) cancellation: finish with `CancellationError`. `finish` is idempotent and
    /// cancels the connection, so a cancel that races completion is a no-op.
    func cancelExternally() {
        queue.async { self.finish(.failure(CancellationError())) }
    }

    private func start() {
        // Everything here runs ON `queue`, not inline from `perform`'s continuation body — the same
        // ordering hazard `CarTransport`'s `HTTPRequest.start()` documents: a task already
        // cancelled on entry can run `onCancel` before `operation` reaches `attach`, and only
        // funnelling both through `queue` keeps `conn`/`selfRetain`/`finished` consistent either way.
        queue.async {
            guard !self.finished else { return }
            self.selfRetain = self               // stay alive until finish(); nobody else holds us
            let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(DongleContract.host),
                                               port: NWEndpoint.Port(rawValue: DongleContract.port)!)
            let c = NWConnection(to: endpoint, using: CarNet.tcpParams())
            self.conn = c
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
            self.queue.asyncAfter(deadline: .now() + self.timeout) { [weak self] in
                guard let self else { return }
                self.finish(.failure(CarError.timeout(self.timeout)))
            }
            c.start(queue: self.queue)
        }
    }

    private func headBytes() -> Data {
        var head = "\(method) \(path) HTTP/1.1\r\n"
        head += "Host: \(DongleContract.host):\(DongleContract.port)\r\n"
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

    /// The body goes out in chunks so `uploadFirmware`'s caller can see progress; for the small
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
        if let completion {
            completion(result)
            self.completion = nil            // idempotence rides on `finished`; this just stops
                                              // holding a used continuation past its use
        } else {
            pendingResult = result
        }
        selfRetain = nil
    }
}
