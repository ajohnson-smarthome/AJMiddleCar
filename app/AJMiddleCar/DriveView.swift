import SwiftUI

struct DriveView: View {
    @ObservedObject var link: CarLink
    @ObservedObject var intent: ControlIntent
    /// `link.video` observed in its own right: `hasPicture` and `fps` change without the
    /// link's own published state moving, and the screen must redraw on them.
    @ObservedObject private var video: VideoLink
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

    /// The car's `video` domain — the switch lives there, and the screen follows the car's
    /// answer, never the tap: a tap that did not land leaves the picture as it was.
    @ObservedObject private var videoCfg = ConfigStore.shared.video
    /// When the last save failed — the button wears `warn` for 600 ms, and that is all the
    /// drive screen says about it.
    @State private var videoToggleFailedAt: Date = .distantPast

    @StateObject private var pad = Gamepad()
    @State private var haptics = Haptics()

    let preview: Bool   // gallery: render statically, no input plumbing
    let previewTricksOpen: Bool   // gallery: the tricks card open, which no CLI can tap open

    init(link: CarLink, intent: ControlIntent, preview: Bool = false, previewTricksOpen: Bool = false) {
        _link = ObservedObject(wrappedValue: link)
        _intent = ObservedObject(wrappedValue: intent)
        _video = ObservedObject(wrappedValue: link.video)
        self.preview = preview
        self.previewTricksOpen = previewTricksOpen
    }

    private var scheme: Scheme { Scheme(rawValue: schemeRaw) ?? .arcade }
    private var p: Palette { Theme.current(colorScheme) }
    private var telemetry: Telemetry? { link.lastTelemetry }
    private var linkUp: Bool { link.isLive }
    private var signalLevel: Int {
        ControlModel.signalLevel(online: linkUp, rssi: telemetry?.link.rssi_dbm,
                                 rxFps: telemetry?.link.rx_hz, expectedFps: CarContract.commandHz)
    }
    private var signalColor: Color { signalLevel == 0 ? .red : (signalLevel == 1 ? p.warn : p.accent) }

    private var screen: DriveScreenState {
        DriveModeRule.state(config: videoCfg.value, covered: showSettings || showCalib)
    }

    /// The video switch: same shape as the gear next to it. The tap posts the whole domain
    /// (bitrate as the car has it), disabled while the answer is on its way.
    private var videoButton: some View {
        let on = videoCfg.value?.enabled ?? false
        let failed = Date().timeIntervalSince(videoToggleFailedAt) < 0.6
        return Button {
            guard let cur = videoCfg.value else { return }
            Task {
                if await !videoCfg.save(Video(bitrate_kbps: cur.bitrate_kbps, enabled: !cur.enabled)) {
                    videoToggleFailedAt = Date()
                    // A state change is what redraws the button: set the mark, and clear
                    // it 650 ms later so the stroke goes back to `line` without a tap.
                    try? await Task.sleep(for: .milliseconds(650))
                    videoToggleFailedAt = .distantPast
                }
            }
        } label: {
            Image(systemName: on ? "video" : "video.slash")
                .font(.system(size: 18, weight: .medium))
                .foregroundStyle(p.text)
                .frame(width: 40, height: 32)
                .background(p.panel)
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(failed ? p.warn : p.line))
        }
        .disabled(videoCfg.value == nil || videoCfg.isBusy)
        .accessibilityLabel(on ? L.videoOn : L.videoOff)
    }

    private var gearButton: some View {
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
        Group {
            if screen.mode == .hud { hud } else { classic }
        }
        .task { await videoCfg.loadIfNeeded() }
        .onAppear { if !preview { video.setWatching(screen.watching) } }
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
        .onDisappear { if !preview { intent.neutral(); video.setWatching(false) } }
        .onChange(of: screen.watching) { _, watching in
            // One gate for all three reasons not to watch — a sheet over the screen, the
            // switch off on the car, the config not read yet (DriveModeRule).
            if !preview { video.setWatching(watching) }
        }
        .onReceive(pad.$leftX) { _ in padPush() }
        .onReceive(pad.$leftY) { _ in padPush() }
        .onReceive(pad.$rightY) { _ in padPush() }
        .onReceive(pad.$connected) { _ in padPush() }
        .sheet(isPresented: $showSettings) { SettingsView(palette: p, link: link) }
        .onChange(of: telemetry?.motors.calibrated) { _, cal in
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

    // The picture is the screen; everything else keeps to its edges. The layout is
    // `DriveLayout`'s — the design's numbers, host-tested — and nothing here sits in the middle
    // of the picture with a scrim behind it: the two gradients from the top and bottom edges are
    // the only tint, and the instruments read against them.
    private var hud: some View {
        GeometryReader { geo in
            let lay = DriveLayout(
                screen: CGSize(width: geo.size.width + geo.safeAreaInsets.leading + geo.safeAreaInsets.trailing,
                               height: geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom),
                insets: .init(top: geo.safeAreaInsets.top, leading: geo.safeAreaInsets.leading,
                              bottom: geo.safeAreaInsets.bottom, trailing: geo.safeAreaInsets.trailing))
            ZStack {
                p.bg.ignoresSafeArea()
                if !preview {
                    // A 16:9 window onto the 4:3 frame: the layer fills it, cropping the top and
                    // bottom eighths — the fisheye's worst — rather than pillarboxing the middle.
                    VideoView(link: video)
                        .frame(width: lay.picture.width, height: lay.picture.height)
                        .clipped()
                        .position(x: lay.picture.midX, y: lay.picture.midY)
                    if !video.hasPicture { noPicture.position(lay.pictureCentre) }
                    scrim(.top)
                    scrim(.bottom)
                }

                HStack(alignment: .center) {
                    HStack(spacing: 12) {
                        HStack(spacing: 7) {
                            SignalBars(level: linkUp ? signalLevel : 0, color: linkUp ? signalColor : .red)
                            // One truth: the label, the bars and the drive screen's existence
                            // all come from `CarLink`, so it cannot say connected while the
                            // joysticks do nothing.
                            Text(linkUp ? L.driveConnected : L.driveSearching)
                                .font(.system(size: 12)).foregroundStyle(p.text)
                        }
                        // The picture's own numbers live next to the link, not in a pill of
                        // their own. `text` rather than `muted`: over the light theme's haze
                        // `muted` disappears.
                        if video.hasPicture {
                            statusItem("video", L.videoStats(fps: video.fps, lost: video.lostLast10s), p.text)
                                .font(.system(size: 10)).opacity(0.8)
                        }
                    }
                    Spacer()
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    videoButton.padding(.leading, 8)
                    gearButton
                }
                .frame(height: 32)
                .padding(.horizontal, lay.edge).padding(.top, 12)
                .frame(maxHeight: .infinity, alignment: .top)

                // The car on the bottom edge, its rails running up over the picture's floor.
                HStack(spacing: 28) {
                    PowerBar(value: sides.left, palette: p)
                    DriveDiagram(t: intent.t, y: intent.y, palette: p)
                    PowerBar(value: sides.right, palette: p)
                }
                .position(lay.diagram)

                // Sticks astride the picture's edges: half on the band, half on the picture's
                // corner, which a fisheye has already darkened.
                if scheme == .arcade {
                    JoystickView(palette: p) { x, y in
                        if arcX == 0 && arcY == 0 && (x != 0 || y != 0) { haptics.tick() }
                        arcX = x; arcY = y; push()
                    }
                    .position(lay.rightStick)
                } else {
                    JoystickView(vertical: true, palette: p) { _, y in leftY = y; push() }.position(lay.leftStick)
                    JoystickView(vertical: true, palette: p) { _, y in rightY = y; push() }.position(lay.rightStick)
                }

                TricksControl(palette: p, running: intent.runningTrick, startedAt: intent.trickStartedAt,
                              onSelect: { intent.startTrick($0) },
                              onStop: { intent.stopTrick() },
                              debugOpen: previewTricksOpen)
                .position(lay.tricks)

                // Warnings are the one thing allowed over the picture, and only while there are
                // any. Under the top row rather than beside it: two at once («драйвер» and
                // «управляет») are wider than the gap between the link and the scheme toggle.
                warnings
                    .padding(.top, 52)
                    .frame(maxHeight: .infinity, alignment: .top)
            }
        }
    }

    /// The screen from before video (81b96ae), for when the car's switch is off: diagram in
    /// the middle, sticks in the corners, tricks and the warnings below. Same components as
    /// the HUD; only the arrangement is its own.
    private var classic: some View {
        ZStack {
            p.bg.ignoresSafeArea()

            VStack {
                HStack {
                    HStack(spacing: 7) {
                        SignalBars(level: linkUp ? signalLevel : 0, color: linkUp ? signalColor : .red)
                        Text(linkUp ? L.driveConnected : L.driveSearching)
                            .font(.system(size: 12)).foregroundStyle(p.muted)
                    }
                    Spacer()
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    videoButton.padding(.leading, 8)
                    gearButton
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
                              onStop: { intent.stopTrick() },
                              debugOpen: previewTricksOpen)
                warnings          // amber only, and only while something is wrong — under the FAB, as before video
            }
            .padding(.bottom, 16)
        }
    }

    /// Shown over the last frame (the layer keeps it) rather than instead of it: the driver
    /// still sees where the car was, and the car still obeys the sticks — a lost picture is
    /// never a stop.
    private var noPicture: some View {
        VStack(spacing: 4) {
            Image(systemName: "video.slash").font(.system(size: 22))
            Text(L.videoNoPicture).font(.system(size: 13, weight: .semibold))
            if let state = telemetry?.video.state, state != .streaming {
                Text(state == .off ? L.videoStateOff : L.videoStateIdle).font(.system(size: 11))
            }
        }
        .foregroundStyle(p.muted)
        .padding(12)
        .background(p.panel.opacity(0.85))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    /// The tint the instruments read against: a gradient from the edge inward, nothing boxed.
    private func scrim(_ edge: VerticalEdge) -> some View {
        let top = edge == .top
        return LinearGradient(colors: [p.bg.opacity(top ? 0.78 : 0), p.bg.opacity(top ? 0 : 0.82)],
                              startPoint: .top, endPoint: .bottom)
            .frame(height: top ? 64 : 130)
            .frame(maxHeight: .infinity, alignment: top ? .top : .bottom)
            .ignoresSafeArea()
            .allowsHitTesting(false)
    }

    /// What `warnings` is about to show — the same conditions its items use. The pill is gated
    /// on it: an empty one with padding and a background is a 16-point dot over the picture.
    private var hasWarnings: Bool {
        (telemetry?.link.timeouts ?? 0) > 0
            || telemetry.map { $0.motors.bus != .ok } ?? false
            || telemetry.map { $0.motors.owner != .remote && $0.motors.owner != .idle } ?? false
    }

    // Only amber, and only while something is wrong: the picture's own numbers moved up next to
    // the link, so with nothing wrong this is empty and the picture is clear.
    private var warnings: some View {
        HStack(spacing: 16) {
            if let trips = telemetry?.link.timeouts, trips > 0 {
                statusItem("exclamationmark.triangle", L.driveWdtTrips(trips), p.warn)
            }
            // A PCA9685 that stopped answering is the one failure that looks exactly like a
            // working car from up here: green pill, green bars, moving diagram, still wheels.
            // The car reports it five times a second, so it gets said.
            if let bus = telemetry?.motors.bus, bus != .ok {
                statusItem("bolt.trianglebadge.exclamationmark", L.driveBusFail, p.warn)
            }
            // The app can be streaming and *not* be the source the car is obeying — a retreat, a
            // calibration pulse or an OTA outranks the pult. Naming the owner is the difference
            // between "the joystick is broken" and "the car is busy doing something else".
            if let owner = telemetry?.motors.owner, owner != .remote, owner != .idle {
                statusItem("hand.raised", L.driveCtlOther(L.ctlOwner(owner.rawValue)), p.warn)
            }
        }
        .font(.system(size: 10))
        .padding(hasWarnings ? 8 : 0)
        .background(hasWarnings ? p.bg.opacity(0.45) : .clear)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
    private func statusItem(_ icon: String, _ text: String, _ color: Color) -> some View {
        HStack(spacing: 4) {
            Image(systemName: icon).foregroundStyle(color.opacity(0.85))
            Text(text).foregroundStyle(color)
        }
    }
}
