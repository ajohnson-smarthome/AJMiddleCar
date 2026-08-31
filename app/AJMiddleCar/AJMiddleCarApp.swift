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
            .onChange(of: scenePhase) { oldPhase, newPhase in
                switch newPhase {
                case .active:
                    // The config prefetch is not here: nothing can be read from a car the app
                    // has not met yet. And the gate decides whether there is a link to open at
                    // all — starting behind the no-internet screen opened an invisible session
                    // that streamed zeros and outranked the bench console.
                    if flow.phase.opensLink { link.start() }
                case .inactive:
                    // Only on the way down. On the way up (.background → .inactive → .active)
                    // the link is already stopped, and a stop enqueued here would cancel the
                    // start the .active step is about to make.
                    if oldPhase == .active {
                        intent.neutral()
                        link.requestStop(graceful: true)
                    }
                case .background:
                    intent.neutral()
                    link.requestStop(graceful: true)
                @unknown default:
                    break
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
        case .checkDongle:
            // Nothing has been asked yet — the default, unadorned radar reads as "checking",
            // the same copy the pre-cutover screen used before there was a dongle sequence in
            // front of it at all.
            ConnectView()
        case .dongleAbsent:
            // Reuses the same "plug it in" copy CarLink's own noDongle situation shows later —
            // whether nothing answered because the interface is not there or because nothing on
            // it has spoken yet is not a distinction worth two screens for the same instruction.
            ConnectView(situation: .noDongle(.notAvailable))
        case .dongleFault:
            ConnectView(situation: .dongleFault)
        case .dongleDenied:
            // The same screen, with the same Settings button, `CarLink` shows for a denial once
            // the gate has handed over. The gate could not say it at all before this: a denied
            // request threw, the throw became nil, and nil said "plug in an adapter".
            ConnectView(situation: .localNetworkDenied)
        case .dongleWrong(let device):
            ConnectView(situation: .wrongDongle(device))
        case .dongleUpdating:
            ConnectView(situation: .dongleUpdating)
        case .dongleUpdateFailed:
            ConnectView(situation: .dongleUpdateFailed, onRetryDongleUpdate: { flow.retryDongleUpdate() })
        case .dongleRolledBack:
            ConnectView(situation: .dongleRolledBack, onSkipRollback: { flow.skipDongleRollback() })
        case .dongleConfiguring:
            ConnectView(situation: .dongleConfiguring)
        case .dongleJoinFailed:
            ConnectView(situation: .dongleJoinFailed, onRetryJoin: { flow.retryDongleJoin() })
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
        case .noDongle(let reason):
            ConnectView(situation: .noDongle(reason))
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
