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
        XCTAssertEqual(RTFrame.command(seq: 7, t: 0.5, y: -1),
                       "{\"seq\":7,\"t\":0.50,\"y\":-1.00}")
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
    func testCalibSaveBody() {
        let a: [Corner: (pair: Int, sign: Int)] = [.fl: (0, 1), .fr: (1, -1), .rl: (2, 1), .rr: (3, -1)]
        XCTAssertEqual(ControlModel.calibSaveBody(a), #"{"wheels":[{"pair":0,"sign":1},{"pair":1,"sign":-1},{"pair":2,"sign":1},{"pair":3,"sign":-1}]}"#)
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
    func testTelemetryParse() {
        let ok = Telemetry.parse("{\"rssi\":-55,\"rx_fps\":10,\"wdt_trips\":2,\"uptime_s\":123,\"heap\":198000,\"calibrated\":true,\"bus_ok\":true,\"ctl\":\"rt\"}")!
        XCTAssertEqual(ok.rssi, -55); XCTAssertEqual(ok.uptimeS, 123); XCTAssertEqual(ok.calibrated, true)
        XCTAssertEqual(ok.rxFps, 10); XCTAssertEqual(ok.busOk, true); XCTAssertEqual(ok.ctl, CtlOwner.rt)
        XCTAssertNil(Telemetry.parse("{\"uptime_s\":1,\"rssi\":0}")!.rssi)
        XCTAssertNil(Telemetry.parse("nope"))
        XCTAssertNil(Telemetry.parse("{\"foo\":1}"))
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
