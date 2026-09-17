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
            // Post-gate guards: once the ladder has handed over (`.awaitingCar`/`.ready`), these
            // are the only signals that send it back. Everywhere mid-ladder, `restart` is a no-op
            // or a fall-back the running stage already owns.
            .onChange(of: link.state) { _, new in
                switch new {
                case .noDongle, .localNetworkDenied: flow.restart(from: .dongle)
                case .wrongCar, .wrongProto:         flow.restart(from: .car)
                case .live:                          flow.carIdentified(fw: link.fw)
                case .searching:                     break
                }
            }
            // Every hand-over to the car re-asks its identity with whatever the link already
            // holds: a hello that landed while the ladder was still deciding was refused by
            // `carIdentified`'s phase guard, and nothing else would ask again — the launch, an
            // adapter that came back, and a forced update that finished all hand over here.
            // `retryAfterWrongCar()` clears the hold on a foreign id/proto — used to run from
            // `WrongCarView`'s own retry, now from here since that screen no longer renders once
            // the ladder has handed over.
            .onChange(of: flow.phase) { _, phase in
                if phase == .awaitingCar {
                    link.retryAfterWrongCar()
                    if link.isLive { flow.carIdentified(fw: link.fw) }
                }
            }
    }

    @ViewBuilder private var root: some View {
        // `shown`, not `phase`: what renders is the paced view of the flow, so a step that
        // resolves in milliseconds still gets its moment instead of strobing past. Decisions
        // elsewhere keep reading `phase`, which is the truth without the pacing.
        switch flow.shown {
        case .stage(let dev, .updating):
            // The forced update: same screen, same phases, same words for either board. Only
            // the object under the chip differs. HTTP only — see `Phase.opensLink`: no session
            // is opened behind it, so as not to shout `wrongProto` at the very board it is
            // updating.
            FirmwareView(palette: p, flow: dev == .car ? .forCar() : .forDongle(client: flow.dongle),
                         forced: true, onDone: { flow.updateFinished(dev) })
        case .stage(let dev, let step):
            ConnectView(situation: .stage(dev, step), onRetry: flow.retryAction(for: step))
        case .releaseCheck:
            ConnectView(situation: .releaseCheck)
        case .releaseOffline:
            ConnectView(situation: .releaseOffline)
        case .releaseMissing(let tag, let dev):
            ConnectView(situation: .releaseMissing(tag: tag, device: dev))
        case .awaitingCar, .ready:
            // The link opens when the ladder hands over, not at launch: until then there is
            // nothing to say to the car, and the ladder is talking to GitHub.
            carRoot.onAppear { link.start() }
        }
    }

    /// Past the ladder, the screen is whatever `CarLink` currently is, except where the ladder is
    /// already back in charge: `.noDongle`, `.localNetworkDenied`, `.wrongCar` and `.wrongProto`
    /// all restarted it through a guard the instant they fired (`.onChange(of: link.state)`
    /// above), so `flow.phase` has already left `.awaitingCar`/`.ready` by the time this would
    /// render one of them — this is a one-frame fallback for that gap, not a second opinion.
    @ViewBuilder private var carRoot: some View {
        switch link.state {
        case .live:
            if flow.phase == .ready {
                DriveView(link: link, intent: intent)
            } else {
                // Live, but the ladder has not answered yet — a moment, not a state (S26).
                ZStack { p.bg.ignoresSafeArea(); ConnectView() }
            }
        default:
            ZStack { p.bg.ignoresSafeArea(); ConnectView() }
        }
    }
}
