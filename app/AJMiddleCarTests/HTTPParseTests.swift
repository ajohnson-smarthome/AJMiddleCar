import XCTest
@testable import AJMiddleCar

final class HTTPParseTests: XCTestCase {
    private func bytes(_ s: String) -> [UInt8] { Array(s.utf8) }

    func testStatusAndContentLength() {
        let head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 17\r\n\r\n"
        let h = HTTPParse.head(bytes(head + "{\"calibrated\":true}"))
        XCTAssertEqual(h?.status, 200)
        XCTAssertEqual(h?.contentLength, 17)
        // In bytes, not Characters: Swift treats "\r\n" as one grapheme, so measuring the head
        // as a String undercounts every line break in it.
        XCTAssertEqual(h?.bodyOffset, head.utf8.count)
    }

    func testHeaderNameIsCaseInsensitive() {
        let h = HTTPParse.head(bytes("HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello"))
        XCTAssertEqual(h?.contentLength, 5)
    }

    func testValueWhitespaceIsTolerated() {
        let h = HTTPParse.head(bytes("HTTP/1.1 200 OK\r\nContent-Length:   42   \r\n\r\n"))
        XCTAssertEqual(h?.contentLength, 42)
    }

    func testNoContentLengthMeansReadToEnd() {
        let h = HTTPParse.head(bytes("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody"))
        XCTAssertEqual(h?.status, 200)
        XCTAssertNil(h?.contentLength)
    }

    func testNonOkStatusIsReportedNotSwallowed() {
        // The clients decide what a 400 means; the parser only reports it.
        let h = HTTPParse.head(bytes("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"))
        XCTAssertEqual(h?.status, 400)
        XCTAssertEqual(h?.contentLength, 0)
    }

    func testReasonPhraseMayBeMissing() {
        let h = HTTPParse.head(bytes("HTTP/1.1 204\r\n\r\n"))
        XCTAssertEqual(h?.status, 204)
    }

    func testIncompleteHeadIsNilSoTheCallerKeepsReading() {
        XCTAssertNil(HTTPParse.head(bytes("HTTP/1.1 200 OK\r\nContent-Length: 17\r\n")))
        XCTAssertNil(HTTPParse.head(bytes("HTTP/1.")))
        XCTAssertNil(HTTPParse.head([]))
    }

    func testGarbageIsRejected() {
        XCTAssertNil(HTTPParse.head(bytes("not http at all\r\n\r\n")))
        XCTAssertNil(HTTPParse.head(bytes("HTTP/1.1 notanumber OK\r\n\r\n")))
    }

    func testBodyOffsetPointsPastTheBlankLine() {
        let raw = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
        let b = bytes(raw)
        guard let h = HTTPParse.head(b) else { return XCTFail("no head") }
        XCTAssertEqual(String(bytes: b[h.bodyOffset...], encoding: .utf8), "ok")
    }
}
