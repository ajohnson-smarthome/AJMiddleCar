import Foundation

/// Typed accessors for localized strings. Text lives in Resources/<lang>.lproj/Localizable.strings.
enum L {
    private static func s(_ key: String, _ args: CVarArg...) -> String {
        let f = NSLocalizedString(key, comment: "")
        return args.isEmpty ? f : String(format: f, arguments: args)
    }
    static var connectTitle: String { s("connect.title") }
    static var connectBody: String { s("connect.body") }
    static var openSettings: String { s("common.openSettings") }
    static var close: String { s("common.close") }
    static var settingsTitle: String { s("settings.title") }
    static var settingsCalibration: String { s("settings.calibration") }
    static var settingsGroupSetup: String { s("settings.groupSetup") }
    static var settingsGroupDriving: String { s("settings.groupDriving") }
    static var settingsGroupSystem: String { s("settings.groupSystem") }
    static var calibTitle: String { s("calib.title") }
    static var calibForward: String { s("calib.forward") }
    static var calibBack: String { s("calib.back") }
    static var calibSpin: String { s("calib.spin") }
    static var calibAllSet: String { s("calib.allSet") }
    static var calibSave: String { s("calib.save") }
    static var calibSpinSub: String { s("calib.spinSub") }
    static var calibWhichDir2: String { s("calib.whichDir2") }
    static var calibDoneTitle: String { s("calib.doneTitle") }
    static var calibSaving: String { s("calib.saving") }
    static var calibSavingSub: String { s("calib.savingSub") }
    static var calibFailTitle: String { s("calib.failTitle") }
    static var calibFailSub: String { s("calib.failSub") }
    static var calibRetry: String { s("calib.retry") }
    static func calibWheel(_ w: String) -> String { s("calib.wheel", w) }
    static var driveSearching: String { s("drive.searching") }
    static var schemeArcade: String { s("scheme.arcade") }
    static var schemeTank: String { s("scheme.tank") }
    static func calibStep(_ n: Int) -> String { s("calib.step", n) }
    static var settingsFirmware: String { s("settings.firmware") }
    static var fwChecking: String { s("fw.checking") }
    static var fwUpToDate: String { s("fw.upToDate") }
    static var fwRecheck: String { s("fw.recheck") }
    static var fwAvailable: String { s("fw.available") }
    static var fwUpdate: String { s("fw.update") }
    static var fwDownloadTitle: String { s("fw.downloadTitle") }
    static var fwConnectTitle: String { s("fw.connectTitle") }
    static var fwConnectSub: String { s("fw.connectSub") }
    static var fwFlash: String { s("fw.flash") }
    static var fwUploadTitle: String { s("fw.uploadTitle") }
    static var fwRebootTitle: String { s("fw.rebootTitle") }
    static var fwRebootWait: String { s("fw.rebootWait") }
    static var fwDoneTitle: String { s("fw.doneTitle") }
    static var fwFailTitle: String { s("fw.failTitle") }
    static var fwFailSub: String { s("fw.failSub") }
    static var fwRetry: String { s("fw.retry") }
    static func fwCurrent(_ v: String) -> String { s("fw.current", v) }
    static func fwVersionLine(_ v: String) -> String { s("fw.versionLine", v) }
    static func fwTransition(_ a: String, _ b: String) -> String { s("fw.transition", a, b) }
    static func fwDoneSub(_ v: String) -> String { s("fw.doneSub", v) }
    static var driveConnected: String { s("drive.connected") }
    static var rampTitle: String { s("ramp.title") }
    static var rampHeadline: String { s("ramp.headline") }
    static var rampSub: String { s("ramp.sub") }
    static var rampValueOff: String { s("ramp.valueOff") }
    static func rampValue(_ ms: Int) -> String { s("ramp.value", ms) }
    static var trimTitle: String { s("trim.title") }
    static var trimSub: String { s("trim.sub") }
    static var trimCenter: String { s("trim.center") }
    static func trimLeft(_ p: Int) -> String { s("trim.left", p) }
    static func trimRight(_ p: Int) -> String { s("trim.right", p) }
    static var recoverTitle: String { s("recover.title") }
    static var recoverHeadline: String { s("recover.headline") }
    static var recoverEnable: String { s("recover.enable") }
    static var recoverWindow: String { s("recover.window") }
    static func recoverWindowValue(_ sec: Int) -> String { s("recover.windowValue", sec) }
    static var recoverSubOn: String { s("recover.subOn") }
    static var recoverSubOff: String { s("recover.subOff") }
    static func trickName(_ key: String) -> String { s(key) }
    static var tricksTitle: String { s("tricks.title") }
    static func trickSec(_ v: Double) -> String { s("tricks.sec", v) }
    static func trickTotal(_ v: Double) -> String { s("tricks.total", v) }
    static func trickCycles(_ n: Int) -> String { s("tricks.cycles", n) }
    static var actFwd: String { s("tricks.fwd") }
    static var actBack: String { s("tricks.back") }
    static var actRight: String { s("tricks.right") }
    static var actLeft: String { s("tricks.left") }
    static var actTurn: String { s("tricks.turn") }
    static func driveWdtTrips(_ n: Int) -> String { s("drive.wdtTrips", n) }
    static var wheelTitle: String { s("wheel.title") }
    static var wheelWizardTitle: String { s("wheel.wizardTitle") }
    static func wheelStep(_ a: Int, _ b: Int) -> String { s("wheel.step", a, b) }
    static var wheelNext: String { s("wheel.next") }
    static var dimsTitle: String { s("dims.title") }
    static var dimsTrack: String { s("dims.track") }
    static var dimsBase: String { s("dims.base") }
    static var dimsTrackHint: String { s("dims.trackHint") }
    static var dimsBaseHint: String { s("dims.baseHint") }
    static var wheelSectionWheels: String { s("wheel.sectionWheels") }
    static var wheelSectionMotors: String { s("wheel.sectionMotors") }
    static var wheelDiameter: String { s("wheel.diameter") }
    static var wheelCirc: String { s("wheel.circ") }
    static var wheelModel: String { s("wheel.model") }
    static var wheelPpr: String { s("wheel.ppr") }
    static var wheelGear: String { s("wheel.gear") }
    static var wheelQuad: String { s("wheel.quad") }
    static var wheelCustom: String { s("wheel.custom") }
    static var mmUnit: String { s("unit.mm") }
    static var rpmUnit: String { s("unit.rpm") }
    static var mUnit: String { s("unit.m") }
    static var cmUnit: String { s("unit.cm") }
    static var simPath: String { s("sim.path") }
    static var simTurns: String { s("sim.turns") }
    static var simArea: String { s("sim.area") }
    static func simVerdict(_ sec: Double, _ turns: Double) -> String { s("sim.verdict", sec, turns) }
    static var simPickMotor: String { s("sim.pickMotor") }
    static var simDiameter: String { s("sim.diameter") }
    static var simCircles: String { s("sim.circles") }
    static var spinTurns: String { s("trick.spinTurns") }
    static var spinDuration: String { s("trick.spinDuration") }
    static var fig8Diameter: String { s("trick.fig8Diameter") }
    static var fig8Loops: String { s("trick.fig8Loops") }
    static var wiggleAmp: String { s("trick.wiggleAmp") }
    static var wiggleCount: String { s("trick.wiggleCount") }
    static var gateNoInternetTitle: String { s("gate.noInternetTitle") }
    static var gateNoInternetSub: String { s("gate.noInternetSub") }
    static var gateCheckFailedTitle: String { s("gate.checkFailedTitle") }
    static var gateCheckFailedSub: String { s("gate.checkFailedSub") }
    static var gateUpdateTitle: String { s("gate.updateTitle") }
    static var gateUpdateSub: String { s("gate.updateSub") }
    static func uptime(_ sec: Int) -> String {
        if sec < 60 { return s("uptime.sec", sec) }
        if sec < 3600 { return s("uptime.min", sec / 60) }
        if sec < 86400 { return s("uptime.hourMin", sec / 3600, (sec % 3600) / 60) }
        return s("uptime.day", sec / 86400)
    }
}
