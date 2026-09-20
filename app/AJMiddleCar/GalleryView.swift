#if DEBUG
import SwiftUI

/// Debug-only screen gallery: every screen/state, tap left/right to navigate. Enabled via the
/// `-gallery` launch argument (see AJMiddleCarApp). Not compiled into release builds.
struct GalleryView: View {
    @Environment(\.colorScheme) private var colorScheme
    /// Seeded from `-galleryIndex N`, so a screenshot run can address a frame directly. The
    /// gallery is otherwise driven by tapping, which a CLI session cannot do — and rebuilding once
    /// per frame to move a hardcoded default is minutes per screen. Taps still work from here.
    @State private var index = Self.seededIndex
    private static var addressed: Bool {
        ProcessInfo.processInfo.arguments.contains("-galleryIndex")
    }
    private static var seededIndex: Int {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: "-galleryIndex"), i + 1 < args.count,
              let n = Int(args[i + 1]) else { return 0 }
        return n
    }
    private var p: Palette { Theme.current(colorScheme) }

    var body: some View {
        let frames = makeFrames(p)
        ZStack {
            p.bg.ignoresSafeArea()
            // .id forces SwiftUI to tear down + recreate on every switch — otherwise same-type frames
            // (FirmwareView/CalibrationView) reuse @State and the .task/.onAppear that seeds
            // debugPhase/debugState never re-runs, so they'd all show one stale state.
            frames[index].view.id(index)
            HStack(spacing: 0) {
                Color.clear.contentShape(Rectangle())
                    .onTapGesture { index = (index - 1 + frames.count) % frames.count }
                Color.clear.contentShape(Rectangle())
                    .onTapGesture { index = (index + 1) % frames.count }
            }
            // Hidden when a frame is addressed explicitly: that is a screenshot run, and the
            // capsule is debug chrome the app itself never shows.
            VStack {
                if !Self.addressed {
                Text("\(index + 1) / \(frames.count)  \u{00B7}  \(frames[index].label)")
                    .font(.system(size: 11, weight: .medium)).monospacedDigit()
                    .foregroundStyle(p.text)
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(Capsule().fill(p.panel.opacity(0.9)))
                    .padding(.top, 8)
                }
                Spacer()
            }
        }
        .statusBarHidden(true)
    }

    /// The pack the drive frames carry unless told otherwise: the spec's «Пак на три четверти».
    nonisolated private static let packOk = BatteryInfo(voltage_mv: 12310, current_ma: 3100, power_mw: 38200,
                                            soc_pct: 72, state: .ok)
    nonisolated private static let packLow = BatteryInfo(voltage_mv: 11100, current_ma: 2600, power_mw: 28900,
                                             soc_pct: 18, state: .low)
    nonisolated private static let packAbsent = BatteryInfo(voltage_mv: nil, current_ma: nil, power_mw: nil,
                                                soc_pct: nil, state: .absent)

    /// A link frozen in `.live` with plausible numbers — the gallery has no transport behind it.
    /// `picture` is what the picture's instrument reads (`VideoLink.preview`): by default 25 fps
    /// without losses, so the instrument is in the drive frames at all — a preview never opens
    /// a socket, so nothing would set `hasPicture` otherwise; `nil` is no picture.
    @MainActor private func mockLink(calibrated: Bool = true, fw: String? = "v1.0+264",
                                     rssi: Int? = -55, wdtTrips: Int = 0,
                                     busOk: Bool = true, owner: MotorsOwner = .remote,
                                     battery: BatteryInfo = Self.packOk,
                                     picture: (fps: Int, lost: Int)? = (25, 0)) -> CarLink {
        let t = Telemetry(proto: CarContract.proto, seq: 1,
                          link: LinkInfo(rx_hz: 10, rssi_dbm: rssi, timeouts: wdtTrips),
                          motors: MotorsInfo(bus: busOk ? .ok : .down, calibrated: calibrated, owner: owner),
                          system: SystemInfo(uptime_s: 3847, free_heap: 131072),
                          video: VideoInfo(state: .idle, fps: 0, kbps: 0, dropped: 0),
                          battery: battery)
        return CarLink.preview(.live(t), fw: fw, radio: .known(RadioInfo(fw: "3.0.6", expected: "3.0.6", state: .ok)),
                               video: picture.map { VideoLink.preview(fps: $0.fps, lost: $0.lost, hasPicture: true) })
    }

    @MainActor private func makeFrames(_ p: Palette) -> [(label: String, view: AnyView)] {
        let intent = ControlIntent()
        // The gallery has no car, and an unread domain honestly renders as "not read" — seed the
        // stores so the settings screens show their controls (one frame keeps the notice).
        ConfigStore.shared.ramp.seed(.default)
        ConfigStore.shared.trim.seed(.default)
        ConfigStore.shared.recovery.seed(.default)
        ConfigStore.shared.wheel.seed(.default)
        ConfigStore.shared.chassis.seed(.default)
        ConfigStore.shared.video.seed(.default)   // the switch on: the drive frames are the HUD
        // One helper, two devices: the whole point of the unification is that these render the
        // same screen with a different object under the chip.
        // The car's frames carry the radio line — `radio` is what `/status` said; the adapter's
        // carry no link and no line.
        func fw(_ phase: FwPhase, forced: Bool = false,
                device: UpdateRules.Device = .car,
                radio: CarLink.RadioStatus? = .known(RadioInfo(fw: "3.0.6", expected: "3.0.6", state: .ok))) -> AnyView {
            let flow = device == .car ? FirmwareFlow.forCar()
                                      : FirmwareFlow.forDongle(client: DongleClient())
            let link = device == .car ? CarLink.preview(.searching, radio: radio) : nil
            return AnyView(NavigationStack {
                FirmwareView(palette: p, flow: flow, link: link, forced: forced, debugPhase: phase)
            })
        }
        func calib(_ d: CalibrationView.CalDebug) -> AnyView {
            AnyView(NavigationStack { CalibrationView(palette: p, debugState: d) })
        }
        return [
            ("Connect (radar)",          AnyView(ConnectView())),
            // Adapter stage
            ("Step 1 finding adapter",   AnyView(ConnectView(situation: .stage(.dongle, .seeking)))),
            ("No dongle",                AnyView(ConnectView(situation: .stage(.dongle, .absent)))),
            ("Checking dongle",          AnyView(ConnectView(situation: .stage(.dongle, .checking)))),
            ("Dongle fault",             AnyView(ConnectView(situation: .stage(.dongle, .fault)))),
            ("Local network denied",     AnyView(ConnectView(situation: .stage(.dongle, .denied), onRetry: {}))),
            ("Wrong dongle",             AnyView(ConnectView(situation: .stage(.dongle, .wrongDevice("some-other-adapter")), onRetry: {}))),
            ("Dongle rolled back",       AnyView(ConnectView(situation: .stage(.dongle, .rolledBack), onRetry: {}))),
            ("Step 3 release check",     AnyView(ConnectView(situation: .releaseCheck))),
            ("Offline, cannot verify",   AnyView(ConnectView(situation: .releaseOffline))),
            ("No release for adapter",   AnyView(ConnectView(situation: .releaseMissing(tag: "v1.0+483", device: .dongle)))),
            ("No release for car",       AnyView(ConnectView(situation: .releaseMissing(tag: "v1.0+483", device: .car)))),
            ("Release without build",    AnyView(ConnectView(situation: .releaseMissing(tag: "v1.0", device: nil)))),
            ("Feed rate-limited",        AnyView(ConnectView(situation: .releaseRefused(retryIn: 1800)))),
            // Reaching the car through the adapter
            ("Dongle sending network",   AnyView(ConnectView(situation: .stage(.car, .sendingNetwork)))),
            ("Step 4 finding car",       AnyView(ConnectView(situation: .stage(.car, .searching)))),
            ("Dongle configuring",       AnyView(ConnectView(situation: .stage(.car, .joining)))),
            ("Dongle join failed",       AnyView(ConnectView(situation: .stage(.car, .joinFailed), onRetry: {}))),
            // Car stage
            ("Car checking",             AnyView(ConnectView(situation: .stage(.car, .seeking)))),
            ("Car fault",                AnyView(ConnectView(situation: .stage(.car, .fault)))),
            ("WrongCar",                 AnyView(ConnectView(situation: .stage(.car, .wrongDevice("esp32-car")), onRetry: {}))),
            ("Car rolled back",          AnyView(ConnectView(situation: .stage(.car, .rolledBack), onRetry: {}))),
            ("Firmware checking",       fw(.checking)),
            ("Firmware upToDate",       fw(.upToDate)),
            ("Firmware available",      fw(.available)),
            ("Firmware downloading",    fw(.downloading)),
            ("Firmware downloaded",     fw(.downloaded)),
            ("Firmware uploading",      fw(.uploading)),
            ("Firmware rebooting",      fw(.rebooting)),
            ("Firmware done",           fw(.done)),
            ("Firmware failed",         fw(.failed)),
            ("Firmware forced",         fw(.available, forced: true)),
            ("Firmware flashed",         fw(.flashed)),
            ("Firmware failed forced",   fw(.failed, forced: true)),
            // The radio line's three words for a car that could not put its own radio right:
            // spent attempt budget, a radio that never answered, and a /status nobody got.
            ("Firmware radio mismatch",  fw(.upToDate, radio: .known(RadioInfo(fw: "3.0.5", expected: "3.0.6", state: .mismatch)))),
            ("Firmware radio silent",    fw(.upToDate, radio: .known(RadioInfo(fw: nil, expected: "3.0.6", state: .unavailable)))),
            ("Firmware radio unknown",   fw(.upToDate, radio: .unavailable)),
            // The adapter, through the same screen — this is the unification, visible.
            ("Adapter fw checking",      fw(.checking, device: .dongle)),
            ("Adapter fw available",     fw(.available, device: .dongle)),
            ("Adapter fw downloading",   fw(.downloading, device: .dongle)),
            ("Adapter fw uploading",     fw(.uploading, device: .dongle)),
            ("Adapter fw rebooting",     fw(.rebooting, device: .dongle)),
            ("Adapter fw done",          fw(.done, device: .dongle)),
            ("Adapter fw failed",        fw(.failed, device: .dongle)),
            ("Adapter fw forced",        fw(.available, forced: true, device: .dongle)),
            ("Adapter fw downloaded",    fw(.downloaded, device: .dongle)),
            ("Adapter fw flashed",       fw(.flashed, device: .dongle)),
            ("Adapter fw failed forced", fw(.failed, forced: true, device: .dongle)),
            ("Drive arcade",            AnyView(DriveView(link: mockLink(), intent: intent, preview: true).onAppear { UserDefaults.standard.set(Scheme.arcade.rawValue, forKey: "scheme") })),
            ("Drive tank",              AnyView(DriveView(link: mockLink(), intent: intent, preview: true).onAppear { UserDefaults.standard.set(Scheme.tank.rawValue, forKey: "scheme") })),
            ("Drive warning",           AnyView(DriveView(link: mockLink(wdtTrips: 3), intent: intent, preview: true))),
            ("Drive bus/ctl warning",   AnyView(DriveView(link: mockLink(busOk: false, owner: .recovering), intent: intent, preview: true))),
            ("Drive tricks open",       AnyView(DriveView(link: mockLink(), intent: intent, preview: true, previewTricksOpen: true))),
            // The same layout with the window empty — the car's switch off: seeded off for this
            // frame only — the gallery seeds the domain on at start, one launch per frame. No
            // picture's readings either: a closed socket publishes none, and the instrument is
            // gone from the row, the pack closed up to the link.
            ("Drive video off",         AnyView(DriveView(link: mockLink(picture: nil), intent: intent, preview: true)
                                            .onAppear { ConfigStore.shared.video.seed(Video(bitrate_kbps: 2500, enabled: false)) })),
            // The pack's two other faces: `low` — the badge in `warn` and the placard under the
            // row; `absent` — an empty icon and «—», and no placard, since a car without a
            // monitor is the norm.
            ("Drive battery low",       AnyView(DriveView(link: mockLink(battery: Self.packLow), intent: intent, preview: true))),
            ("Drive battery absent",    AnyView(DriveView(link: mockLink(battery: Self.packAbsent), intent: intent, preview: true))),
            // The picture losing frames: the spec's «Кадры теряются» — «22 к/с» in `text`, and
            // the pair «пунктирный кадр · 7» in `warn` beside it; the neighbours keep their colours.
            ("Drive picture lost",      AnyView(DriveView(link: mockLink(picture: (22, 7)), intent: intent, preview: true))),
            ("Settings",                AnyView(NavigationStack { SettingsView(palette: p, link: mockLink()) })),
            ("Calibration spin",        calib(.spin)),
            ("Calibration spinning",    calib(.spinning)),
            ("Calibration spin failed", calib(.spinFailed)),
            ("Calibration direction",   calib(.direction)),
            ("Calibration done",        calib(.done)),
            ("Calibration saving",      calib(.saving)),
            ("Calibration failed",      calib(.failed)),
            ("Ramp",                    AnyView(NavigationStack { RampView(palette: p) })),
            ("Trim",                    AnyView(NavigationStack { TrimView(palette: p) })),
            ("Recover",                 AnyView(NavigationStack { RecoverView(palette: p) })),
            ("Car dimensions",          AnyView(NavigationStack { CarDimensionsView(palette: p, wizard: true) })),
            ("Wheel & motors",          AnyView(NavigationStack { WheelParamsView(palette: p) })),
            ("Tricks settings",         AnyView(NavigationStack { TricksSettingsView(palette: p) })),
            ("Video settings",          AnyView(NavigationStack { VideoSettingsView(palette: p) })),
            ("Config not read",         AnyView(NavigationStack { RampView(palette: p) }
                                            .onAppear { ConfigStore.shared.ramp.seedUnknown() })),
        ]
    }
}
#endif
