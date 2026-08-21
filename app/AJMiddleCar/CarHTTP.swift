import Foundation
import Network

/// The car's REST calls, over a connection bound to Wi-Fi.
///
/// This exists because `URLSession` cannot bind a request to an interface, and the car lives on a
/// network iOS refuses to use for general traffic (see `CarNet`). It is deliberately the smallest
/// HTTP/1.1 client that serves these endpoints: one connection per request, `Connection: close`,
/// no reuse, no redirects, no chunked encoding. Every REST call here is a settings screen or a
/// one-shot probe — the 10 Hz control stream rides `CarConnection` instead — so nothing is gained
/// by making it cleverer.
enum CarHTTP {
    @discardableResult
    static func get(_ path: String, timeout: TimeInterval = 3) async -> (status: Int, body: Data)? {
        await send("GET", path, nil, nil, timeout)
    }

    /// `progress` reports 0...1 as the body goes out, which the firmware upload screen needs —
    /// it is the one request here big enough for a user to watch.
    @discardableResult
    static func post(_ path: String,
                     body: Data,
                     contentType: String = "application/json",
                     timeout: TimeInterval = 5,
                     progress: (@Sendable (Double) -> Void)? = nil) async -> (status: Int, body: Data)? {
        await send("POST", path, body, contentType, timeout, progress)
    }

    private static func send(_ method: String,
                             _ path: String,
                             _ body: Data?,
                             _ contentType: String?,
                             _ timeout: TimeInterval,
                             _ progress: (@Sendable (Double) -> Void)? = nil) async -> (status: Int, body: Data)? {
        await withCheckedContinuation { cont in
            Request(method: method, path: path, body: body, contentType: contentType,
                    timeout: timeout, progress: progress) { cont.resume(returning: $0) }
                .start()
        }
    }
}

/// One request, start to finish. All state is touched only on `queue`, and `finish` is idempotent
/// so the deadline and the connection callbacks can race without resuming the continuation twice.
private final class Request: @unchecked Sendable {
    private let method: String
    private let path: String
    private let body: Data?
    private let contentType: String?
    private let timeout: TimeInterval
    private let progress: (@Sendable (Double) -> Void)?
    private let done: ((status: Int, body: Data)?) -> Void

    private let queue = DispatchQueue(label: "carhttp")
    private var conn: NWConnection?
    private var buf: [UInt8] = []
    private var finished = false
    private var self_retain: Request?

    init(method: String, path: String, body: Data?, contentType: String?,
         timeout: TimeInterval, progress: (@Sendable (Double) -> Void)?,
         done: @escaping ((status: Int, body: Data)?) -> Void) {
        self.method = method
        self.path = path
        self.body = body
        self.contentType = contentType
        self.timeout = timeout
        self.progress = progress
        self.done = done
    }

    func start() {
        self_retain = self                      // stay alive until finish(), nobody else holds us
        let c = NWConnection(to: CarNet.endpoint(), using: CarNet.params(webSocket: false))
        conn = c
        c.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.write()
            case .failed, .cancelled:
                self.finish(nil)
            case .waiting:
                // A path that never becomes available is exactly the failure this class exists to
                // survive, so waiting is left to the deadline rather than treated as progress.
                break
            default:
                break
            }
        }
        queue.asyncAfter(deadline: .now() + timeout) { [weak self] in self?.finish(nil) }
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
            if err != nil { self.finish(nil); return }
            self.writeBody(from: 0)
        })
    }

    /// The body goes out in chunks so the firmware upload can report progress; for the small JSON
    /// bodies everything else sends, this is a single pass.
    private func writeBody(from offset: Int) {
        guard let body, offset < body.count else { read(); return }
        let end = min(offset + 32 * 1024, body.count)
        conn?.send(content: body.subdata(in: offset..<end), completion: .contentProcessed {
            [weak self] err in
            guard let self else { return }
            if err != nil { self.finish(nil); return }
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
                    self.finish((head.status, self.slice(head.bodyOffset, want)))
                    return
                }
                if isComplete || error != nil {
                    self.finish((head.status, self.slice(head.bodyOffset, available)))
                    return
                }
            } else if isComplete || error != nil {
                self.finish(nil)                // stream ended without a parseable response
                return
            }
            self.read()
        }
    }

    private func slice(_ offset: Int, _ count: Int) -> Data {
        guard count > 0, offset + count <= buf.count else { return Data() }
        return Data(buf[offset..<(offset + count)])
    }

    private func finish(_ result: (status: Int, body: Data)?) {
        guard !finished else { return }
        finished = true
        conn?.cancel()
        conn = nil
        done(result)
        self_retain = nil
    }
}
