import Foundation

/// Pure HTTP/1.1 response head parsing — no Network, no I/O, host-tested with `swiftc`.
///
/// Splitting a response is the only part of the car's HTTP client with real edge cases, so it
/// lives apart from the socket glue that only a device can exercise.
enum HTTPParse {
    struct Head: Equatable {
        let status: Int
        /// nil when the response carries no Content-Length; the body then runs to end-of-stream.
        let contentLength: Int?
        /// Index of the first body byte in the buffer the head was parsed from.
        let bodyOffset: Int
    }

    /// Parse the status line and headers.
    ///
    /// Returns nil when the buffer does not yet hold a complete head (the caller should read more)
    /// or when the status line is not HTTP. Both are answered the same way on purpose: a caller
    /// that keeps reading until end-of-stream will end up failing either way, and distinguishing
    /// "not yet" from "never" would buy nothing at these sizes.
    static func head(_ bytes: [UInt8]) -> Head? {
        guard let sep = findDoubleCRLF(bytes) else { return nil }
        let headBytes = Array(bytes[0..<sep])
        guard let text = String(bytes: headBytes, encoding: .utf8) else { return nil }

        var lines = text.components(separatedBy: "\r\n")
        guard let statusLine = lines.first else { return nil }
        lines.removeFirst()

        // "HTTP/1.1 200 OK" — the reason phrase is optional and ignored.
        let parts = statusLine.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true)
        guard parts.count >= 2, parts[0].hasPrefix("HTTP/"), let status = Int(parts[1]) else {
            return nil
        }

        var length: Int?
        for line in lines {
            guard let colon = line.firstIndex(of: ":") else { continue }
            let name = line[line.startIndex..<colon].trimmingCharacters(in: .whitespaces).lowercased()
            guard name == "content-length" else { continue }
            let value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            length = Int(value)
        }

        return Head(status: status, contentLength: length, bodyOffset: sep + 4)
    }

    private static func findDoubleCRLF(_ b: [UInt8]) -> Int? {
        guard b.count >= 4 else { return nil }
        for i in 0...(b.count - 4) where b[i] == 13 && b[i + 1] == 10 && b[i + 2] == 13 && b[i + 3] == 10 {
            return i
        }
        return nil
    }
}
