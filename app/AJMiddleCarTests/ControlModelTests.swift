import XCTest
@testable import AJMiddleCar

final class ControlModelTests: XCTestCase {
    private func close(_ a: Double, _ b: Double) -> Bool { abs(a - b) < 1e-6 }

    func testArcadeForward() {
        let r = ControlModel.arcade(stickX: 0, stickY: -1)
        XCTAssertTrue(close(r.t, 1) && close(r.y, 0))
    }
    func testArcadeTurn() {
        let r = ControlModel.arcade(stickX: 1, stickY: 0)
        XCTAssertTrue(close(r.t, 0) && close(r.y, 1))
    }
    func testTankForward() {
        let r = ControlModel.tank(leftStickY: -1, rightStickY: -1)
        XCTAssertTrue(close(r.t, 1) && close(r.y, 0))
    }
    func testTankSpin() {
        let r = ControlModel.tank(leftStickY: -1, rightStickY: 1)
        XCTAssertTrue(close(r.t, 0) && close(r.y, 1))
    }
    func testClamp() {
        XCTAssertEqual(ControlModel.clamp(2.5), 1)
        XCTAssertEqual(ControlModel.clamp(-2.5), -1)
        XCTAssertEqual(ControlModel.clamp(0.3), 0.3)
    }
    // The frame itself is host-tested in app/tests/rtframe; this only guards the wiring.
    func testFrame() {
        XCTAssertEqual(RTFrame.command(seq: 7, throttle: 0.5, turn: -1),
                       #"{"proto":2,"type":"drive","seq":7,"throttle":0.50,"turn":-1.00}"#)
    }
    func testSidesForward() {
        let s = ControlModel.sides(t: 1, y: 0)
        XCTAssertTrue(close(s.left, 1) && close(s.right, 1))
    }
    func testSidesSpin() {
        let s = ControlModel.sides(t: 0, y: 1)
        XCTAssertTrue(close(s.left, 1) && close(s.right, -1))
    }
    func testSidesArcNormalized() {
        let s = ControlModel.sides(t: 0.5, y: 0.5)
        XCTAssertTrue(close(s.left, 1) && close(s.right, 0))
    }
    func testDiagramState() {
        XCTAssertEqual(ControlModel.diagramState(t: 0.8, y: 0), .drive)
        XCTAssertEqual(ControlModel.diagramState(t: 0, y: 0.7), .spin)
        XCTAssertEqual(ControlModel.diagramState(t: 0, y: 0), .idle)
    }
    func testCurvature() {
        XCTAssertEqual(ControlModel.curvature(t: 1, y: 0), 0, accuracy: 1e-9)
        XCTAssertTrue(ControlModel.curvature(t: 1, y: 0.5) > 0)
        XCTAssertTrue(ControlModel.curvature(t: 1, y: -0.5) < 0)
    }
    func testTrajectoryStraightVsCurved() {
        XCTAssertLessThan(abs(ControlModel.trajectoryPoints(t: 1, y: 0, length: 100, steps: 24).last!.x), 1e-6)
        XCTAssertGreaterThan(abs(ControlModel.trajectoryPoints(t: 1, y: 0.6, length: 100, steps: 24).last!.x), 5)
    }
    func testTrajectoryNeverLoops() {
        // extreme small-t / large-y must stay a gentle arc, never curl back (y strictly decreasing)
        let ex = ControlModel.trajectoryPoints(t: 0.08, y: 1, length: 120, steps: 24)
        for i in 1..<ex.count { XCTAssertLessThan(ex[i].y, ex[i - 1].y) }
    }
    func testCalibWheels() {
        let a: [Corner: (pair: Int, inverted: Bool)] = [.fl: (0, false), .fr: (1, true), .rl: (2, false), .rr: (3, true)]
        XCTAssertEqual(ControlModel.calibWheels(a), [CalibWheel(corner: .front_left, pair: 0, inverted: false),
                                                     CalibWheel(corner: .front_right, pair: 1, inverted: true),
                                                     CalibWheel(corner: .rear_left, pair: 2, inverted: false),
                                                     CalibWheel(corner: .rear_right, pair: 3, inverted: true)])
    }
    func testSignalLevelRssi() {
        let fps = CarContract.commandHz
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: -45, rxFps: 0, expectedFps: fps), 4)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: -55, rxFps: 0, expectedFps: fps), 3)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: -65, rxFps: 0, expectedFps: fps), 2)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: -80, rxFps: 10, expectedFps: fps), 1)
        XCTAssertEqual(ControlModel.signalLevel(online: false, rssi: -45, rxFps: 10, expectedFps: fps), 0)
    }
    /// A car that cannot read its AP station list reports rssi 0; a live link must never render
    /// as empty red bars because of it.
    func testSignalLevelFallsBackToRxFps() {
        let fps = CarContract.commandHz
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: nil, rxFps: 10, expectedFps: fps), 4)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: 0, rxFps: 10, expectedFps: fps), 4)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: nil, rxFps: 8, expectedFps: fps), 3)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: nil, rxFps: 5, expectedFps: fps), 2)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: nil, rxFps: 2, expectedFps: fps), 1)
        XCTAssertEqual(ControlModel.signalLevel(online: true, rssi: nil, rxFps: nil, expectedFps: fps), 1)
        XCTAssertEqual(ControlModel.signalLevel(online: false, rssi: nil, rxFps: 10, expectedFps: fps), 0)
    }
    // Telemetry is now the generated Codable struct decoded straight off the wire; this only
    // guards the wiring (the shape itself is covered by app/tests/carapi).
    func testTelemetryParse() {
        let json = #"{"proto":2,"seq":1,"link":{"rx_hz":10,"rssi_dbm":-55,"timeouts":2},"motors":{"bus":"ok","calibrated":true,"owner":"remote"},"system":{"uptime_s":123,"free_heap":198000}}"#
        let ok = try! JSONDecoder().decode(Telemetry.self, from: Data(json.utf8))
        XCTAssertEqual(ok.link.rssi_dbm, -55); XCTAssertEqual(ok.system.uptime_s, 123); XCTAssertEqual(ok.motors.calibrated, true)
        XCTAssertEqual(ok.link.rx_hz, 10); XCTAssertEqual(ok.motors.bus, .ok); XCTAssertEqual(ok.motors.owner, .remote)
        // rssi_dbm is nullable on the wire — this is what "unmeasured" looks like.
        let noRssiJson = #"{"proto":2,"seq":1,"link":{"rx_hz":0,"rssi_dbm":null,"timeouts":0},"motors":{"bus":"ok","calibrated":false,"owner":"idle"},"system":{"uptime_s":1,"free_heap":0}}"#
        XCTAssertNil(try! JSONDecoder().decode(Telemetry.self, from: Data(noRssiJson.utf8)).link.rssi_dbm)
        XCTAssertNil(try? JSONDecoder().decode(Telemetry.self, from: Data("nope".utf8)))
        XCTAssertNil(try? JSONDecoder().decode(Telemetry.self, from: Data(#"{"foo":1}"#.utf8)))
    }
    @MainActor func testBuildNumberAndUpdate() {
        XCTAssertEqual(UpdateClient.buildNumber("v1.2+246"), 246)
        XCTAssertNil(UpdateClient.buildNumber("v1.0"))
        XCTAssertEqual(UpdateClient.buildNumber("v1.2+246-dirty"), 246)
        XCTAssertTrue(UpdateClient.isUpdateAvailable(running: "v1.0+246", latest: "v1.0+250"))
        XCTAssertFalse(UpdateClient.isUpdateAvailable(running: "v1.0+250", latest: "v1.0+250"))
        XCTAssertFalse(UpdateClient.isUpdateAvailable(running: "v1.0+250", latest: "v1.0+240"))
        XCTAssertTrue(UpdateClient.isUpdateAvailable(running: "v0.9", latest: "v1.0"))
    }
    @MainActor func testGateLogic() {
        XCTAssertFalse(UpdateClient.needsDownload(latestBuild: nil, cachedBuild: nil, hasCachedFile: false))
        XCTAssertTrue(UpdateClient.needsDownload(latestBuild: 254, cachedBuild: nil, hasCachedFile: false))
        XCTAssertFalse(UpdateClient.needsDownload(latestBuild: 254, cachedBuild: 254, hasCachedFile: true))
        XCTAssertTrue(UpdateClient.needsDownload(latestBuild: 260, cachedBuild: 254, hasCachedFile: true))
        XCTAssertTrue(UpdateClient.needsDownload(latestBuild: 254, cachedBuild: 254, hasCachedFile: false))
        XCTAssertFalse(UpdateClient.mustUpdate(carFw: "v1.0+250", latestTag: "v1.0"))
        XCTAssertTrue(UpdateClient.mustUpdate(carFw: "v0.9", latestTag: "v1.0+254"))
        XCTAssertFalse(UpdateClient.mustUpdate(carFw: "v1.0+254", latestTag: "v1.0+254"))
        XCTAssertTrue(UpdateClient.mustUpdate(carFw: "v1.0+250", latestTag: "v1.0+254"))
        XCTAssertFalse(UpdateClient.mustUpdate(carFw: "v1.0+260", latestTag: "v1.0+254"))
    }
}
