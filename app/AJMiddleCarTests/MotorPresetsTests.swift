import XCTest
@testable import AJMiddleCar

final class MotorPresetsTests: XCTestCase {
    func testCpr() {
        // JGB37-520B: 11 PPR · 1:9 · ×4 → 396
        XCTAssertEqual(MotorPresets.cpr(ppr: 11, gearX100: 900, quad: 4), 396, accuracy: 0.001)
        // generic high gearing: 11 PPR · 1:21 · ×4 → 924
        XCTAssertEqual(MotorPresets.cpr(ppr: 11, gearX100: 2100, quad: 4), 924, accuracy: 0.001)
        // fractional gear 1:9.6, ×2 → 211.2
        XCTAssertEqual(MotorPresets.cpr(ppr: 11, gearX100: 960, quad: 2), 211.2, accuracy: 0.001)
    }
    func testAllIsTheSinglePreset() {
        XCTAssertEqual(MotorPresets.all.map { $0.id }, ["jgb37-520b-1000"])
    }
    func testPresetCpr() {
        XCTAssertEqual(MotorPresets.all.first { $0.id == "jgb37-520b-1000" }?.cpr ?? 0, 396, accuracy: 0.001)
    }
    func testMatch() {
        XCTAssertEqual(MotorPresets.match(ppr: 11, gearX100: 900, quad: 4)?.name, "JGB37-520B")
        // hand-entered values that fit no preset — including the retired motor's gearing
        XCTAssertNil(MotorPresets.match(ppr: 11, gearX100: 2100, quad: 4))
        XCTAssertNil(MotorPresets.match(ppr: 13, gearX100: 2100, quad: 4))
    }
    func testIdsUnique() {
        XCTAssertEqual(Set(MotorPresets.all.map { $0.id }).count, MotorPresets.all.count)
    }
}
