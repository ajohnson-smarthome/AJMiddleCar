import SwiftUI

@main
struct AJMiddleCarApp: App {
    @StateObject private var link = CarLink()
    @StateObject private var intent = ControlIntent()
    @StateObject private var flow = AppFlow()
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.colorScheme) private var colorScheme
    private var p: Palette { Theme.current(colorScheme) }

    var body: some Scene {
        WindowGroup {
            #if DEBUG
            if ProcessInfo.processInfo.arguments.contains("-gallery") {
                GalleryView()
            } else {
                appRoot
            }
            #else
            appRoot
            #endif
        }
    }

    private var appRoot: some View {
        root
            .statusBarHidden(true)
            .persistentSystemOverlays(.hidden)
            .task {
                await flow.startupCheck()
                // The car may have identified itself while the gate was still talking to
                // GitHub; `onChange` would have missed a value that arrived before the phase
                // was ready to hear it.
                flow.carIdentified(fw: link.fw)
            }
            .onChange(of: scenePhase) { _, newPhase in
                if newPhase == .active {
                    // The config prefetch is not here: nothing can be read from a car the app has
                    // not met yet, and both GETs would time out into `.failed` with nothing to
                    // retrigger them. `CarLink` warms them the moment the session is adopted.
                    link.start()
                } else {
                    // Leaving `.active` is a goodbye said in words. The car stops in under
                    // 150 ms instead of noticing silence 300 ms later and retreating along its
                    // own path with the controls off-screen.
                    intent.neutral()
                    Task { await link.stop(graceful: true) }
                }
            }
            .onChange(of: link.fw) { _, fw in flow.carIdentified(fw: fw) }
            .onChange(of: link.state) { _, _ in
                // The identity may already be known when the gate finishes; re-asking is cheap
                // and closes the race where the hello landed before `startupCheck` returned.
                if case .live = link.state { flow.carIdentified(fw: link.fw) }
            }
    }

    @ViewBuilder private var root: some View {
        switch flow.phase {
        case .checkInternet, .checkUpdate, .downloading, .checkFailed:
            UpdateCheckView(palette: p, phase: flow.phase, client: flow.client) { flow.retry() }
        case .noInternet:
            NoInternetView(palette: p) { flow.retry() }
        case .updateRequired:
            FirmwareView(palette: p, forced: true, onDone: { flow.updateFinished() }, link: link)
                .onAppear { link.start() }
        case .awaitingCar, .ready:
            // The link opens when the gate hands over, not at launch: until then there is
            // nothing to say to the car, and the gate is talking to GitHub.
            carRoot.onAppear { link.start() }
        }
    }

    /// Past the gate, the screen is whatever `CarLink` currently is. There is no second opinion.
    @ViewBuilder private var carRoot: some View {
        switch link.state {
        case .noWiFi(let reason):
            ConnectView(situation: .noWiFi(reason))
        case .localNetworkDenied:
            ConnectView(situation: .localNetworkDenied)
        case .wrongCar(let device):
            WrongCarView(palette: p, kind: .foreignDevice(device)) { link.retryAfterWrongCar() }
        case .wrongProto(let theirs):
            WrongCarView(palette: p, kind: .protoMismatch(theirs: theirs)) { link.retryAfterWrongCar() }
        case .searching:
            ZStack { p.bg.ignoresSafeArea(); ConnectView() }
        case .live:
            if flow.phase == .ready {
                DriveView(link: link, intent: intent)
            } else {
                // Live, but the version gate has not answered yet — a moment, not a state.
                ZStack { p.bg.ignoresSafeArea(); ConnectView() }
            }
        }
    }
}
