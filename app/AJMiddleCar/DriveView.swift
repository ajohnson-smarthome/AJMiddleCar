import SwiftUI

struct DriveView: View {
    @ObservedObject var conn: CarConnection
    @ObservedObject var status: CarStatus
    @Environment(\.colorScheme) private var colorScheme
    @AppStorage("scheme") private var schemeRaw = Scheme.arcade.rawValue

    @State private var arcX = 0.0
    @State private var arcY = 0.0
    @State private var leftY = 0.0
    @State private var rightY = 0.0
    @State private var curT = 0.0
    @State private var curY = 0.0
    @State private var showSettings = false
    @State private var showCalib = false
    @State private var lastCalibTrue = Date.distantPast
    @State private var runningTrick: Trick?
    @State private var trickStartedAt: Date?
    @State private var trickTask: Task<Void, Never>?

    @StateObject private var pad = Gamepad()
    @State private var haptics = Haptics()

    let preview: Bool   // gallery: render statically, skip network start

    init(conn: CarConnection, status: CarStatus, preview: Bool = false) {
        _conn = ObservedObject(wrappedValue: conn)
        _status = ObservedObject(wrappedValue: status)
        self.preview = preview
    }

    private var scheme: Scheme { Scheme(rawValue: schemeRaw) ?? .arcade }
    private var p: Palette { Theme.current(colorScheme) }
    private var signalLevel: Int { ControlModel.signalLevel(online: status.online, rssi: status.rssi, pingMs: nil) }
    private var signalColor: Color { signalLevel == 0 ? .red : (signalLevel == 1 ? p.warn : p.accent) }
    private var linkUp: Bool { status.online }

    private func push() {
        let c: (t: Double, y: Double)
        let padActive = pad.connected && (abs(pad.leftX) > 0.03 || abs(pad.leftY) > 0.03 || abs(pad.rightY) > 0.03)
        if padActive { manualOverride() }   // genuine gamepad deflection → drop any running trick
        if padActive {
            if scheme == .arcade { c = ControlModel.arcade(stickX: pad.leftX, stickY: -pad.leftY) }
            else { c = ControlModel.tank(leftStickY: -pad.leftY, rightStickY: -pad.rightY) }
        } else if scheme == .arcade {
            c = ControlModel.arcade(stickX: arcX, stickY: arcY)
        } else {
            c = ControlModel.tank(leftStickY: leftY, rightStickY: rightY)
        }
        curT = c.t; curY = c.y
        conn.setCommand(ControlModel.frame(t: c.t, y: c.y))
    }

    private var sides: (left: Double, right: Double) { ControlModel.sides(t: curT, y: curY) }

    private func startTrick(_ base: Trick) {
        trickTask?.cancel()
        trickTask = Task {
            let trick: Trick
            if base.id == Tricks.spin.id {
                // Turns + duration from settings; speed derived at the real motor speed / track.
                let vmax = await donutVmaxMS()
                if Task.isCancelled { return }
                let track = await donutTrackM()
                if Task.isCancelled { return }
                trick = Tricks.spinTrick(turns: TrickSettings.spinTurns(), durationMs: TrickSettings.spinDurMs(),
                                         vmaxMS: vmax, trackM: track)
            } else if base.id == Tricks.donut.id {
                // The donut's (t,y) comes from the diameter; its duration from the circle count,
                // timed at the real motor speed (nominal fallback when /wheel is unavailable).
                let vmax = await donutVmaxMS()
                if Task.isCancelled { return }
                let track = await donutTrackM()
                if Task.isCancelled { return }                 // cancelled during the fetches → bail cleanly
                trick = Tricks.donutTrick(diameterCm: Double(TrickSettings.donutDiameterCm()),
                                          circles: TrickSettings.donutCircles(), vmaxMS: vmax, trackM: track)
            } else if base.id == Tricks.figure8.id {
                // Two tangent loops sized by diameter; per-lobe duration from the circle solver at real speed.
                let vmax = await donutVmaxMS()
                if Task.isCancelled { return }
                let track = await donutTrackM()
                if Task.isCancelled { return }
                trick = Tricks.figure8Trick(diameterCm: Double(TrickSettings.fig8Dia()),
                                            eights: TrickSettings.fig8Eights(), vmaxMS: vmax, trackM: track)
            } else if base.id == Tricks.wiggle.id {
                // In-place oscillation — no geometry, build synchronously from amplitude + wag count.
                trick = Tricks.wiggleTrick(amplitude: TrickSettings.wiggleAmp(), wags: TrickSettings.wiggleWags())
            } else {
                trick = Tricks.withDurations(base, TrickSettings.durations(for: base))  // per-action durations
            }
            runningTrick = trick
            trickStartedAt = Date()                            // totalMs drives the ring
            for step in trick.steps {
                conn.setCommand(ControlModel.frame(t: step.t, y: step.y))
                curT = step.t; curY = step.y                  // drive the on-screen diagram/power bars
                try? await Task.sleep(nanoseconds: UInt64(step.ms) * 1_000_000)
                if Task.isCancelled { return }
            }
            conn.setCommand(ControlModel.frame(t: 0, y: 0))   // natural end → stop
            curT = 0; curY = 0
            runningTrick = nil; trickStartedAt = nil
        }
    }

    /// Linear speed (m/s) from the car's wheel/motor params, with a nominal fallback.
    private func donutVmaxMS() async -> Double {
        guard let w = await WheelClient().get(),
              let rpm = MotorPresets.match(ppr: w.ppr, gearX100: w.gearX100, quad: w.quad)?.rpm
        else { return Tricks.donutNominalVmaxMS }
        return Double.pi * (Double(w.diameterMm) / 1000) * Double(rpm) / 60
    }

    /// Track (m) from the car's /dims, with the nominal fallback.
    private func donutTrackM() async -> Double {
        guard let d = await DimsClient().get() else { return Tricks.donutTrackFallbackM }
        return Double(d.trackMm) / 1000
    }

    private func cancelTrick(stop: Bool) {
        trickTask?.cancel(); trickTask = nil; runningTrick = nil; trickStartedAt = nil
        curT = 0; curY = 0                                    // diagram back to idle (joystick reasserts if taking over)
        if stop { conn.setCommand(ControlModel.frame(t: 0, y: 0)) }
        // stop == false: leave the command — the joystick is about to set it (seamless takeover)
    }

    /// Genuine manual input → drop any running trick without stopping (joystick takes over).
    private func manualOverride() { if runningTrick != nil { cancelTrick(stop: false) } }

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()

            VStack {
                HStack {
                    HStack(spacing: 7) {
                        SignalBars(level: linkUp ? signalLevel : 0, color: linkUp ? signalColor : .red)
                        // "Connected" requires BOTH a reachable /status AND a live WS control link —
                        // otherwise the joysticks would silently do nothing while the pill says connected.
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
                DriveDiagram(t: curT, y: curY, palette: p)
                PowerBar(value: sides.right, palette: p)
            }

            if scheme == .arcade {
                HStack {
                    Spacer()
                    JoystickView(palette: p) { x, y in
                        manualOverride()
                        if arcX == 0 && arcY == 0 && (x != 0 || y != 0) { haptics.tick() }
                        arcX = x; arcY = y; push()
                    }
                    .padding(.trailing, 24)
                }
                .padding(.bottom, 16)
                .frame(maxHeight: .infinity, alignment: .bottom)
            } else {
                HStack {
                    JoystickView(vertical: true, palette: p) { _, y in manualOverride(); leftY = y; push() }.padding(.leading, 24)
                    Spacer()
                    JoystickView(vertical: true, palette: p) { _, y in manualOverride(); rightY = y; push() }.padding(.trailing, 24)
                }
                .padding(.bottom, 16)
                .frame(maxHeight: .infinity, alignment: .bottom)
            }

            VStack(spacing: 6) {
                Spacer()
                TricksControl(palette: p, running: runningTrick, startedAt: trickStartedAt,
                              onSelect: { startTrick($0) },
                              onStop: { cancelTrick(stop: true) })
                statusBar          // «Обрывов: N» sits directly under the FAB
            }
            .padding(.bottom, 16)
        }
        .onAppear { conn.onTelemetry = { status.apply($0) }; if !preview { conn.start(); status.start() } }
        .onDisappear { trickTask?.cancel() }
        .onReceive(pad.$leftX) { _ in push() }
        .onReceive(pad.$leftY) { _ in push() }
        .onReceive(pad.$rightY) { _ in push() }
        .onReceive(pad.$connected) { _ in push() }
        .sheet(isPresented: $showSettings) { SettingsView(palette: p, status: status) }
        .onReceive(status.$calibrated) { cal in
            if cal == true {
                showCalib = false                       // calibrated → close
                lastCalibTrue = Date()
            } else if cal == false, Date().timeIntervalSince(lastCalibTrue) > 2 {
                // Mandatory: reopen — but ignore the stale `false` that /status still reports for
                // up to one poll (~1.5s) right after a successful save, which would re-open the
                // sheet mid-dismiss and flicker.
                showCalib = true
            }
        }
        .sheet(isPresented: $showCalib) {
            NavigationStack {
                CarDimensionsView(palette: p, wizard: true)  // step 1 → Wheel → Calibration
            }
            .interactiveDismissDisabled(true)
        }
    }

    // Empty in the normal case: only amber warnings ever appear here.
    private var statusBar: some View {
        HStack(spacing: 16) {
            if let trips = status.wdtTrips, trips > 0 {
                statusItem("exclamationmark.triangle", L.driveWdtTrips(trips), p.warn)
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
