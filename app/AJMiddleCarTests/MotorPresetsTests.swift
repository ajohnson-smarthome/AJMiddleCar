import XCTest
@testable import ESP32Car

final class MotorPresetsTests: XCTestCase {
    func testCpr() {
        XCTAssertEqual(MotorPresets.cpr(ppr: 11, gearX100: 2100, quad: 4), 924, accuracy: 0.001)
        XCTAssertEqual(MotorPresets.cpr(ppr: 11, gearX100: 900, quad: 4), 396, accuracy: 0.001)
        XCTAssertEqual(MotorPresets.cpr(ppr: 11, gearX100: 960, quad: 2), 211.2, accuracy: 0.001)
    }
    func testPresetCpr() {
        XCTAssertEqual(MotorPresets.all.first { $0.id == "jga25-370-170" }?.cpr ?? 0, 924, accuracy: 0.001)
        XCTAssertEqual(MotorPresets.all.first { $0.id == "jgb37-520b-1000" }?.cpr ?? 0, 396, accuracy: 0.001)
    }
    func testMatch() {
        XCTAssertEqual(MotorPresets.match(ppr: 11, gearX100: 2100, quad: 4)?.name, "JGA25-370")
        XCTAssertEqual(MotorPresets.match(ppr: 11, gearX100: 900, quad: 4)?.name, "JGB37-520B")
        XCTAssertNil(MotorPresets.match(ppr: 13, gearX100: 2100, quad: 4))
    }
    func testIdsUnique() {
        XCTAssertEqual(Set(MotorPresets.all.map { $0.id }).count, MotorPresets.all.count)
    }
}
