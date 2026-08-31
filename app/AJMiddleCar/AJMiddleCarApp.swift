import SwiftUI

@main
struct AJMiddleCarApp: App {
    @StateObject private var link = CarLink()
    @StateObject private var intent = ControlIntent()
    @StateObject private var flow = AppFlow()

    var body: some Scene {
        WindowGroup {
            #if DEBUG
            if ProcessInfo.processInfo.arguments.contains("-gallery") {
                GalleryView()
            } else {
                RootView(link: link, intent: intent, flow: flow)
            }
            #else
            RootView(link: link, intent: intent, flow: flow)
            #endif
        }
    }
}

/// Everything the app shows, and the reason it is a `View` rather than properties on the `App`.
///
/// `@Environment(\.colorScheme)` declared on an `App` does not track the window's appearance: it
/// reads a default and never updates. The palette derived from it was therefore always the light
/// one, and every screen handed that palette from above — the firmware screen, the update check,
/// settings, the no-internet screen — rendered light inside a dark app, while `ConnectView` and
/// `DriveView`, which read the environment themselves from inside the hierarchy, rendered
/// correctly. A launch would go dark, light, dark as it moved between them.
///
/// `scenePhase` genuinely does work at `App` level; it moved here only to keep the two together.
struct RootView: View {
    @ObservedObject var link: CarLink
    @ObservedObject var intent: ControlIntent
    @ObservedObject var flow: AppFlow
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.colorScheme) private var colorScheme
    private var p: Palette { Theme.current(colorScheme) }

    var body: some View { appRoot }

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
                    if flow.shown.opensLink { link.start() }
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
            .onChange(of: link.state) { old, new in
                // The identity may already be known when the gate finishes; re-asking is cheap
                // and closes the race where the hello landed before `startupCheck` returned.
                if case .live = new { flow.carIdentified(fw: link.fw) }
                // The wire came back. Everything the dongle was asked at launch has to be asked
                // again — above all whether it is still joined to the car — and the gate that
                // asked returned for good when it handed over. Without this, a dongle replugged
                // after the car was switched off answers "failed" to nobody at all.
                if old.isNoDongle, !new.isNoDongle {
                    Task { await flow.dongleReturned() }
                }
            }
    }

    @ViewBuilder private var root: some View {
        // `shown`, not `phase`: what renders is the paced view of the flow, so a step that
        // resolves in milliseconds still gets its moment instead of strobing past. Decisions
        // elsewhere keep reading `phase`, which is the truth without the pacing.
        switch flow.shown {
        // Steps 1 and 2 of the ladder. Both draw the same adapter; the first draws it faint and
        // the second makes it solid, which is the whole reason they are two screens and not one
        // — the movement between them IS the information. Whether nothing has answered yet or
        // nothing is there is not a distinction worth separate copy, so both land on step 1.
        case .checkDongle, .dongleAbsent:
            ConnectView(situation: .findingAdapter)
        case .dongleChecking:
            ConnectView(situation: .checkingDongle)
        case .dongleUpdateCheck:
            ConnectView(situation: .adapterUpdateCheck)
        case .carFinding:
            ConnectView(situation: .findingCar)
        case .dongleOffline:
            ConnectView(situation: .offline)
        case .dongleNoRelease(let tag):
            ConnectView(situation: .noRelease(tag: tag))
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
            // The adapter's update is the car's update: same screen, same phases, same words.
            // Only the object under the chip differs.
            FirmwareView(palette: p, flow: .forDongle(client: flow.dongle), forced: true,
                         onDone: { flow.dongleUpdateFinished() })
        case .dongleRolledBack:
            ConnectView(situation: .dongleRolledBack,
                        onRecheckRollback: { flow.recheckDongleRollback() })
        case .dongleConfiguring:
            ConnectView(situation: .dongleConfiguring)
        case .dongleJoinFailed:
            ConnectView(situation: .dongleJoinFailed, onRetryJoin: { flow.retryDongleJoin() })
        // Step 6 of the ladder: the car's own release check, which now says what it is doing
        // and to whom. `UpdateCheckView` keeps the two states that are not a step — a download
        // with a progress bar, and a failure with a button.
        case .checkInternet, .checkUpdate:
            ConnectView(situation: .carUpdateCheck)
        case .downloading, .checkFailed:
            UpdateCheckView(palette: p, phase: flow.shown, client: flow.client) { flow.retry() }
        case .noInternet:
            NoInternetView(palette: p) { flow.retry() }
        case .updateRequired:
            FirmwareView(palette: p, flow: .forCar(link: link), forced: true,
                         onDone: { flow.updateFinished() })
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
