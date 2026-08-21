import SwiftUI

struct DriveView: View {
    @ObservedObject var link: CarLink
    @ObservedObject var intent: ControlIntent
    @Environment(\.colorScheme) private var colorScheme
    @AppStorage("scheme") private var schemeRaw = Scheme.arcade.rawValue

    @State private var arcX = 0.0
    @State private var arcY = 0.0
    @State private var leftY = 0.0
    @State private var rightY = 0.0
    @State private var showSettings = false
    @State private var showCalib = false
    @State private var lastCalibTrue = Date.distantPast
    @State private var padWasActive = false

    @StateObject private var pad = Gamepad()
    @State private var haptics = Haptics()

    let preview: Bool   // gallery: render statically, no input plumbing

    init(link: CarLink, intent: ControlIntent, preview: Bool = false) {
        _link = ObservedObject(wrappedValue: link)
        _intent = ObservedObject(wrappedValue: intent)
        self.preview = preview
    }

    private var scheme: Scheme { Scheme(rawValue: schemeRaw) ?? .arcade }
    private var p: Palette { Theme.current(colorScheme) }
    private var telemetry: Telemetry? { link.lastTelemetry }
    private var linkUp: Bool { link.isLive }
    private var signalLevel: Int {
        ControlModel.signalLevel(online: linkUp, rssi: telemetry?.rssi,
                                 rxFps: telemetry?.rxFps, expectedFps: CarContract.commandHz)
    }
    private var signalColor: Color { signalLevel == 0 ? .red : (signalLevel == 1 ? p.warn : p.accent) }

    /// The simulator reports a phantom controller that is always "connected", so an idle stick
    /// must not count as input — it would otherwise mask touch and pre-empt a running trick with
    /// a zero command.
    private var padActive: Bool {
        pad.connected && (abs(pad.leftX) > 0.03 || abs(pad.leftY) > 0.03 || abs(pad.rightY) > 0.03)
    }

    /// Every input path lands here, and `ControlIntent` decides what it means for a running
    /// trick — the view no longer orders "cancel the trick" and "send the command" by hand.
    private func push() {
        guard !preview else { return }
        let c: (t: Double, y: Double)
        if padActive {
            if scheme == .arcade { c = ControlModel.arcade(stickX: pad.leftX, stickY: -pad.leftY) }
            else { c = ControlModel.tank(leftStickY: -pad.leftY, rightStickY: -pad.rightY) }
        } else if scheme == .arcade {
            c = ControlModel.arcade(stickX: arcX, stickY: arcY)
        } else {
            c = ControlModel.tank(leftStickY: leftY, rightStickY: rightY)
        }
        intent.manual(t: c.t, y: c.y)
    }

    /// A gamepad event is input only while the sticks are deflected — plus the one event that
    /// brings them back to centre, which is how the car learns to stop.
    private func padPush() {
        let active = padActive
        defer { padWasActive = active }
        if active || padWasActive { push() }
    }

    private var sides: (left: Double, right: Double) {
        ControlModel.sides(t: intent.t, y: intent.y)
    }

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()

            VStack {
                HStack {
                    HStack(spacing: 7) {
                        SignalBars(level: linkUp ? signalLevel : 0, color: linkUp ? signalColor : .red)
                        // One truth: the pill, the bars and the drive screen's existence all
                        // come from `CarLink`, so the pill cannot say connected while the
                        // joysticks do nothing.
                        Text(linkUp ? L.driveConnected : L.driveSearching)
                            .font(.system(size: 12)).foregroundStyle(p.muted)
                    }
                    Spacer()
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    Button { showSettings = true } label: {
                        Image(systemName: "gearshape")
                            .font(.system(size: 18, weight: .medium))
                            .foregroundStyle(p.text)
                            .frame(width: 40, height: 32)
                            .background(p.panel)
                            .clipShape(RoundedRectangle(cornerRadius: 10))
                            .overlay(RoundedRectangle(cornerRadius: 10).stroke(p.line))
                    }
                    .padding(.leading, 8)
                    .disabled(showCalib)   // can't bypass mandatory calibration via Settings
                }
                .padding(.horizontal, 18).padding(.top, 8)
                Spacer()
            }

            HStack(spacing: 28) {
                PowerBar(value: sides.left, palette: p)
                DriveDiagram(t: intent.t, y: intent.y, palette: p)
                PowerBar(value: sides.right, palette: p)
            }

            if scheme == .arcade {
                HStack {
                    Spacer()
                    JoystickView(palette: p) { x, y in
                        if arcX == 0 && arcY == 0 && (x != 0 || y != 0) { haptics.tick() }
                        arcX = x; arcY = y; push()
                    }
                    .padding(.trailing, 24)
                }
                .padding(.bottom, 16)
                .frame(maxHeight: .infinity, alignment: .bottom)
            } else {
                HStack {
                    JoystickView(vertical: true, palette: p) { _, y in leftY = y; push() }.padding(.leading, 24)
                    Spacer()
                    JoystickView(vertical: true, palette: p) { _, y in rightY = y; push() }.padding(.trailing, 24)
                }
                .padding(.bottom, 16)
                .frame(maxHeight: .infinity, alignment: .bottom)
            }

            VStack(spacing: 6) {
                Spacer()
                TricksControl(palette: p, running: intent.runningTrick, startedAt: intent.trickStartedAt,
                              onSelect: { intent.startTrick($0) },
                              onStop: { intent.stopTrick() })
                statusBar          // «Обрывов: N» sits directly under the FAB
            }
            .padding(.bottom, 16)
        }
        // Zero the intent, and deliberately do NOT say goodbye here.
        //
        // The plan lists a bye "when the drive screen is dismissed", written for a screen the
        // user leaves on purpose. This one has none: it is dismissed only because `link.state`
        // stopped being `.live` — a second of stale telemetry does it — and `link.stop()` there
        // would be unrecoverable, because the only callers of `link.start()` are the scene
        // becoming `.active` and `carRoot.onAppear`, and neither fires again while the app stays
        // in the foreground on `.ready`. One dropped telemetry frame would end the drive.
        //
        // Nothing is lost by leaving it out. The transport keeps streaming `t:0, y:0` at
        // `commandHz`, which feeds the car's control watchdog and so suppresses the retreat the
        // bye exists to suppress; ownership is worth nothing to hold onto, because the car adopts
        // whichever peer says hello next; and OTA outranks RT in the car's own arbitration
        // (`link.h`: `LINK_SRC_OTA > LINK_SRC_RT`), so a streaming pult cannot lock out a flash.
        // The two real departures — the scene leaving `.active`, and teardown — do send it.
        .onDisappear { if !preview { intent.neutral() } }
        .onReceive(pad.$leftX) { _ in padPush() }
        .onReceive(pad.$leftY) { _ in padPush() }
        .onReceive(pad.$rightY) { _ in padPush() }
        .onReceive(pad.$connected) { _ in padPush() }
        .sheet(isPresented: $showSettings) { SettingsView(palette: p, link: link) }
        .onChange(of: telemetry?.calibrated) { _, cal in
            if cal == true {
                showCalib = false                       // calibrated → close
                lastCalibTrue = Date()
            } else if cal == false, Date().timeIntervalSince(lastCalibTrue) > 2, !preview {
                // Mandatory: reopen — but ignore the stale `false` the car still reports for a
                // frame or two right after a successful save, which would re-open the sheet
                // mid-dismiss and flicker.
                showCalib = true
            }
        }
        .sheet(isPresented: $showCalib, onDismiss: {
            // The wizard is interactiveDismissDisabled, so the only way it closes is its own
            // dismiss() after a save the car accepted. Treat that as "calibrated": the telemetry
            // frame already in flight was computed before the write and still says false.
            lastCalibTrue = Date()
        }) {
            NavigationStack {
                CarDimensionsView(palette: p, wizard: true)  // step 1 → Wheel → Calibration
            }
            .interactiveDismissDisabled(true)
        }
    }

    // Empty in the normal case: only amber warnings ever appear here.
    private var statusBar: some View {
        HStack(spacing: 16) {
            if let trips = telemetry?.wdtTrips, trips > 0 {
                statusItem("exclamationmark.triangle", L.driveWdtTrips(trips), p.warn)
            }
            // A PCA9685 that stopped answering is the one failure that looks exactly like a
            // working car from up here: green pill, green bars, moving diagram, still wheels.
            // The car reports it five times a second, so it gets said.
            if telemetry?.busOk == false {
                statusItem("bolt.trianglebadge.exclamationmark", L.driveBusFail, p.warn)
            }
            // The app can be streaming and *not* be the source the car is obeying — a retreat, a
            // calibration pulse or an OTA outranks the pult. Naming the owner is the difference
            // between "the joystick is broken" and "the car is busy doing something else".
            if let owner = telemetry?.ctl, owner != CtlOwner.rt, owner != CtlOwner.none {
                statusItem("hand.raised", L.driveCtlOther(L.ctlOwner(owner)), p.warn)
            }
        }
        .font(.system(size: 10))
    }
    private func statusItem(_ icon: String, _ text: String, _ color: Color) -> some View {
        HStack(spacing: 4) {
            Image(systemName: icon).foregroundStyle(color.opacity(0.85))
            Text(text).foregroundStyle(color)
        }
    }
}
