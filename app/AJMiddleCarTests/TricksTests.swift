import XCTest
@testable import ESP32Car

final class TricksTests: XCTestCase {
    func testBaseFiveSeconds() {
        for tr in Tricks.all {
            XCTAssertFalse(tr.steps.isEmpty)
            XCTAssertEqual(tr.totalMs, 5000)
            for s in tr.steps { XCTAssertTrue(s.t >= -1 && s.t <= 1 && s.y >= -1 && s.y <= 1 && s.ms > 0) }
        }
    }
    func testIdsUnique() { XCTAssertEqual(Set(Tricks.all.map { $0.id }).count, Tricks.all.count) }
    func testDistinctActions() {
        XCTAssertEqual(Tricks.distinctActions(Tricks.spin).count, 1)
        XCTAssertEqual(Tricks.distinctActions(Tricks.figure8).count, 2)
        let w = Tricks.distinctActions(Tricks.wiggle)
        XCTAssertEqual(w.count, 2)
        XCTAssertEqual(w[0].count, 10); XCTAssertEqual(w[1].count, 10)
    }
    func testBaseDurations() {
        XCTAssertEqual(Tricks.baseDurations(Tricks.figure8), [2500, 2500])
        XCTAssertEqual(Tricks.baseDurations(Tricks.wiggle), [250, 250])
    }
    func testWithDurations() {
        let t = Tricks.withDurations(Tricks.wiggle, [400, 100])
        XCTAssertEqual(t.steps.count, 20)
        XCTAssertEqual(t.steps[0].ms, 400); XCTAssertEqual(t.steps[1].ms, 100)
        XCTAssertEqual(t.totalMs, 10 * 400 + 10 * 100)
        XCTAssertEqual(Tricks.withDurations(Tricks.spin, [99]).steps[0].ms, 100)   // clamp
        XCTAssertEqual(Tricks.withDurations(Tricks.spin, [1, 2]).totalMs, 5000)     // wrong length → base
    }
    func testDescriptor() {
        XCTAssertTrue(Tricks.actionDescriptor(0, 1) == (0, 1))
        XCTAssertTrue(Tricks.actionDescriptor(0.6, -0.6) == (1, -1))
    }
}
