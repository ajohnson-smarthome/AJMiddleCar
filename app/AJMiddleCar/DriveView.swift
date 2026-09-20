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
    /// The mandatory wizard's rule (`CalibGate`): asked on every telemetry frame, and
    /// `showCalib` only mirrors its verdict. State of this view, so it is born with the
    /// session's drive screen and dies with it — no memory of a previous session.
    @State private var calib: CalibGate
    @State private var padWasActive = false

    /// The car's `video` domain — the switch lives there.
    @ObservedObject private var videoCfg = ConfigStore.shared.video
    /// For the one thing the store itself says: the car's settings were reset at its boot
    /// (`resetNotice`), shown here once, until tapped away.
    @ObservedObject private var config = ConfigStore.shared
    /// The switch as the car last confirmed it — deliberately not `videoCfg.value`: the store
    /// shows what it is *sending* while a save is in flight and nothing at all after a failed
    /// one, and the window must light up and go dark on the car's answer alone
    /// (`app/drive-hud`): a tap that did not land leaves the picture as it was, and never
    /// strands the driver with an empty window and a dead button while the car still
    /// streams. Fed from the store's `.loaded` state only.
    @State private var confirmed: Video?
    /// The last save failed — the button wears `warn` for 600 ms, and that is all the drive
    /// screen says about it.
    @State private var videoToggleFailed = false

    @StateObject private var pad = Gamepad()
    @State private var haptics = Haptics()

    let preview: Bool   // gallery: render statically, no input plumbing
    let previewTricksOpen: Bool   // gallery: the tricks card open, which no CLI can tap open

    init(link: CarLink, intent: ControlIntent, preview: Bool = false, previewTricksOpen: Bool = false) {
        _link = ObservedObject(wrappedValue: link)
        _intent = ObservedObject(wrappedValue: intent)
        _video = ObservedObject(wrappedValue: link.video)
        // Seeded here, not only on appear: the domain is prefetched when the car is met, so
        // the first body already lights the window rather than drawing it empty for a frame.
        if case .loaded(let v) = ConfigStore.shared.video.state { _confirmed = State(initialValue: v) }
        _calib = State(initialValue: CalibGate(preview: preview))
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

    /// Something drawn over the drive screen: a sheet — settings or the mandatory wizard — or
    /// the «Поиск…» veil of a telemetry pause inside a live session. One notion for the rule,
    /// because the three mean the same thing to the car: nobody is driving it right now.
    private var covered: Bool { showSettings || showCalib || !linkUp }

    private var screen: DriveScreenState {
        DriveModeRule.state(config: confirmed, covered: covered)
    }

    /// Whether the window holds a picture at all: the car confirmed the switch on. Not
    /// `screen.watching` — a sheet over the screen lapses the subscription, but the window
    /// under it stays live, so the last frame is still there when the sheet closes. Off, or
    /// the domain not read yet, the window is empty: no layer, no «Нет картинки» — that is
    /// the driver's choice, and the video button's glyph says so.
    private var windowLive: Bool { confirmed?.enabled == true }

    /// The video switch: the middle segment of the control bar, a glyph in a slot the bar
    /// frames. Read, the tap posts the whole domain (bitrate as the car has it); unread, it
    /// re-reads — a dead button over an unread domain left the driver with an empty window and
    /// no way back to the picture but the settings sheet (AJM-120). Disabled only while an
    /// answer is on its way. A refusal is said by this segment's own stroke wearing `warn`
    /// for 650 ms, inside the bar's outline — the neighbours do not change.
    private var videoButton: some View {
        let on = confirmed?.enabled ?? false
        let glyph = confirmed == nil ? "questionmark.video" : (on ? "video" : "video.slash")
        return Button {
            Task {
                let landed: Bool
                switch DriveModeRule.videoTap(config: confirmed) {
                case .reread:
                    await videoCfg.reload()
                    landed = videoCfg.value != nil
                case .toggle(let next):
                    landed = await videoCfg.save(next)
                    // The store is `.failed` now and would refuse a retry; re-read the car's
                    // truth behind the flash. `confirmed` moves only if the GET lands — if it
                    // fails too, the last answer stands and the button stays live for a retry.
                    if !landed { Task { await videoCfg.reload() } }
                }
                if !landed {
                    videoToggleFailed = true
                    try? await Task.sleep(for: .milliseconds(650))
                    videoToggleFailed = false
                }
            }
        } label: {
            segmentGlyph(glyph)
                .overlay {
                    if videoToggleFailed {
                        RoundedRectangle(cornerRadius: 8).stroke(p.warn).padding(2)
                    }
                }
        }
        .disabled(videoCfg.isBusy)
        .accessibilityLabel(confirmed == nil ? L.configRetry : (on ? L.videoOn : L.videoOff))
    }

    /// The settings segment: the bar's rightmost.
    private var gearButton: some View {
        Button { showSettings = true } label: { segmentGlyph("gearshape") }
            .disabled(showCalib)   // can't bypass mandatory calibration via Settings
    }

    /// A bar segment's label: the glyph in `text`, filling the slot so the whole segment
    /// takes the tap. No frame of its own — the bar draws the body and the outline.
    private func segmentGlyph(_ name: String) -> some View {
        Image(systemName: name)
            .font(.system(size: 18, weight: .medium))
            .foregroundStyle(p.text)
            .frame(width: ControlBarSegment.size.width, height: ControlBarSegment.size.height)
            .contentShape(Rectangle())
    }

    /// The simulator reports a phantom controller that is always "connected", so an idle stick
    /// must not count as input — it would otherwise mask touch and pre-empt a running trick with
    /// a zero command.
    private var padActive: Bool {
        pad.connected && (abs(pad.leftX) > 0.03 || abs(pad.leftY) > 0.03 || abs(pad.rightY) > 0.03)
    }

    /// Every input path lands here, and `ControlIntent` decides what it means for a running
    /// trick — the view no longer orders "cancel the trick" and "send the command" by hand.
    /// Covered, it lands nowhere: the sheet has the screen, and a gamepad the sheet cannot
    /// cover has no say either (AJM-101) — the rule, not each input site, decides.
    private func push() {
        guard !preview, screen.inputAllowed else { return }
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

    // One layout: the window is the screen, and everything else keeps to its edges whether or
    // not there is a picture in it. The places are `DriveLayout`'s — derived from the screen
    // and its safe area, host-tested — and nothing here sits in the middle of the window with
    // a scrim behind it: the two gradients from the top and bottom edges are the only tint,
    // and the instruments read against them. The car's switch decides only whether the
    // window is live; no instrument moves on it.
    var body: some View {
        GeometryReader { geo in
            let lay = DriveLayout(
                screen: CGSize(width: geo.size.width + geo.safeAreaInsets.leading + geo.safeAreaInsets.trailing,
                               height: geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom),
                insets: .init(top: geo.safeAreaInsets.top, leading: geo.safeAreaInsets.leading,
                              bottom: geo.safeAreaInsets.bottom, trailing: geo.safeAreaInsets.trailing))
            ZStack {
                p.bg.ignoresSafeArea()
                if !preview && windowLive {
                    // A 16:9 window for a frame the car already cropped to 16:9 (the fisheye's
                    // top and bottom eighths never reach the wire): the layer fills it rather
                    // than pillarboxing, so only a squatter screen trims anything, at the sides.
                    // Mounted only while the window is live: the switch turned off must not
                    // leave the last frame hanging, and the layer forgets it on dismantle.
                    VideoView(link: video)
                        .frame(width: lay.picture.width, height: lay.picture.height)
                        .clipped()
                        .position(x: lay.picture.midX, y: lay.picture.midY)
                    if !video.hasPicture { noPicture.position(lay.pictureCentre) }
                }
                // Always, picture or not: over the plain background they are invisible, and the
                // instruments read the same the moment a frame appears — nothing rebuilds.
                scrim(.top)
                scrim(.bottom)

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
                        // The pack, from the same frame: always there, since a car without a
                        // monitor is the empty icon, not a gap in the row.
                        BatteryBadge(battery: telemetry?.battery, palette: p)
                    }
                    Spacer()
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    // Tricks · video · settings in one capsule; the tricks card opens below it.
                    ControlBar(palette: p) {
                        TricksControl(palette: p, running: intent.runningTrick, startedAt: intent.trickStartedAt,
                                      onSelect: { intent.startTrick($0) },
                                      onStop: { intent.stopTrick() },
                                      debugOpen: previewTricksOpen)
                    } video: {
                        videoButton
                    } settings: {
                        gearButton
                    }
                    .padding(.leading, 8)
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

                // Sticks astride the window's edges: half on the band, half on the picture's
                // corner, which a fisheye has already darkened. Arcade leaves the left place
                // empty — tank fills it — so nothing wanders between the schemes.
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

                // Warnings are the one thing allowed over the picture, and only while there are
                // any. Under the top row rather than beside it: two at once («драйвер» and
                // «управляет») are wider than the gap between the link and the scheme toggle.
                notices
                    .padding(.top, 52)
                    .frame(maxHeight: .infinity, alignment: .top)

                // A telemetry pause inside a live session is drawn over the screen, not instead
                // of it: the root keeps this view while the session stands (`CarLink.inSession`),
                // so the sheets on it — the wizard mid-table, the settings stack — survive what
                // used to be a swap to the radar and back (AJM-107). The veil also covers the
                // screen for the rule above: nothing under it drives.
                if !linkUp && !preview { searchingVeil }
            }
        }
        .task { if !preview { await videoCfg.loadIfNeeded() } }
        .onChange(of: videoCfg.state, initial: true) { old, st in
            guard case .loaded(let v) = st else { return }
            // The car took a write that changes a running stream's bitrate — the slider on the
            // settings sheet. It reads the bitrate at stream start only, so the subscription
            // has to lapse before the next `view`, or the sheet's closing refreshes the old
            // stream and the change is invisible (AJM-122). Judged on the write the store just
            // finished (`.saving` → `.loaded`), against the value the car last confirmed.
            if case .saving = old, let was = confirmed, !preview,
               VideoReopenHold.restartsStream(from: was, to: v) {
                video.streamRestartNeeded()
            }
            confirmed = v
        }
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
        .onChange(of: screen.inputAllowed) { _, allowed in
            guard !preview else { return }
            if allowed {
                // The cover is gone: input is taken again from where the controls are now. The
                // gamepad's axes are live values and are re-read here; the touch stick's are
                // not — a finger that was on it when the sheet came got no `onEnded`, so its
                // last position was zeroed with the cover, and the next drag event re-seeds it.
                padPush()
            } else {
                // Covered: the held command goes to zero now — a trick with it — rather than
                // when the gesture happens to end, which under a sheet it may never report.
                intent.neutral()
                arcX = 0; arcY = 0; leftY = 0; rightY = 0
            }
        }
        .onReceive(pad.$leftX) { _ in padPush() }
        .onReceive(pad.$leftY) { _ in padPush() }
        .onReceive(pad.$rightY) { _ in padPush() }
        .onReceive(pad.$connected) { _ in padPush() }
        .sheet(isPresented: $showSettings) { SettingsView(palette: p, link: link) }
        // Every frame, `initial: true` — the first one included: this screen appears with a
        // frame already in hand, and an uncalibrated car's `false` in it is no change for an
        // `onChange` on the flag to see, which is how the mandatory wizard used to stay shut on
        // exactly the car it exists for (AJM-63). The rule judges the frame; the flag's history
        // is its business, not this view's.
        .onChange(of: telemetry, initial: true) { _, t in
            let required = calib.frame(calibrated: t?.motors.calibrated, bus: t?.motors.bus,
                                       now: Date().timeIntervalSinceReferenceDate)
            if showCalib != required { showCalib = required }
        }
        .sheet(isPresented: $showCalib) {
            NavigationStack {
                CarDimensionsView(palette: p, wizard: true)  // step 1 → Wheel → Calibration
            }
            .interactiveDismissDisabled(true)
            // The wizard is interactiveDismissDisabled, so it closes two ways: telemetry says
            // `calibrated:true` (the rule above), or the car accepted the table and the wizard
            // says so here. Its own `dismiss()` cannot do it — inside a `NavigationStack` that
            // is a pop back to step 2 with «Далее», and the sheet then hung on the next frame
            // of telemetry to close (AJM-102). Closing here, the rule is told at once: the
            // frame already in flight was computed before the write and still says false.
            .environment(\.calibSaved) {
                calib.saved(now: Date().timeIntervalSinceReferenceDate)
                showCalib = false
            }
        }
        // Outermost, so both sheets inherit it: the wizard reads `motors.bus` from here and
        // shows «Драйвер моторов не отвечает» instead of «Крутить» while the boards are not
        // answering (AJM-93) — the mandatory one has closed by then (`CalibGate`), the one
        // reached through settings stays and says why.
        .environment(\.motorBus, telemetry?.motors.bus)
    }

    /// «Поиск…» over the drive screen: telemetry is late, the session is not over. Translucent,
    /// so the driver still sees where the car was; opaque to touches, so nothing under it is
    /// tapped by mistake — the rule already refuses input while covered, this keeps the buttons
    /// honest too. The label and the bars in the top row say the same thing.
    private var searchingVeil: some View {
        ZStack {
            p.bg.opacity(0.55).ignoresSafeArea()
            HStack(spacing: 8) {
                ProgressView().tint(p.muted)
                Text(L.driveSearching).font(.system(size: 14, weight: .semibold)).foregroundStyle(p.text)
            }
            .padding(.horizontal, 16).padding(.vertical, 10)
            .background(p.panel.opacity(0.9))
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.line))
        }
    }

    /// Shown over the last frame (the layer keeps it) rather than instead of it: the driver
    /// still sees where the car was, and the car still obeys the sticks — a lost picture is
    /// never a stop. Only while the window is live: an empty window is not a lost picture.
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

    /// The telemetry warnings, and under them the one notice that is the app's rather than
    /// the frame's: the car's store was wiped at its boot, so its settings and calibration are
    /// at their defaults (`app/settings`, AJM-99). Said once per car boot, until tapped away;
    /// it waits under the mandatory wizard the same wipe opens, for when that closes.
    private var notices: some View {
        VStack(spacing: 6) {
            warnings
            if config.resetNotice {
                Button { config.dismissResetNotice() } label: {
                    HStack(spacing: 6) {
                        statusItem("arrow.counterclockwise.circle", L.configResetNotice, p.warn)
                        Image(systemName: "xmark").foregroundStyle(p.warn.opacity(0.7))
                    }
                    .font(.system(size: 10))
                    .padding(8)
                    .background(p.bg.opacity(0.45))
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                .buttonStyle(.plain)
            }
        }
    }

    /// What `warnings` is about to show — the same conditions its items use. The pill is gated
    /// on it: an empty one with padding and a background is a 16-point dot over the picture.
    private var hasWarnings: Bool {
        (telemetry?.link.timeouts ?? 0) > 0
            || telemetry.map { $0.motors.bus != .ok } ?? false
            || telemetry.map { $0.motors.owner != .remote && $0.motors.owner != .idle } ?? false
            || telemetry.map { $0.battery.state == .low } ?? false
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
            // Only `low`: a car without a monitor is the badge's empty icon, not a warning.
            if telemetry?.battery.state == .low {
                statusItem("battery.25", L.batteryLow, p.warn)
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
