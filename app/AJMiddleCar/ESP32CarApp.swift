import SwiftUI

@main
struct ESP32CarApp: App {
    @StateObject private var conn = CarConnection()
    @StateObject private var status = CarStatus()
    @StateObject private var flow = AppFlow()
    @Environment(\.scenePhase) private var phase
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
            // Forward WS telemetry to CarStatus in every phase (not just DriveView) so the
            // car stays "online" during the connect/update gates, not only while driving.
            .task { conn.onTelemetry = { status.apply($0) }; await flow.startupCheck() }
            .onChange(of: phase) { newPhase in
                if newPhase == .active { conn.resume(); status.start() }
                else { conn.pause(); status.stop() }
            }
    }

    @ViewBuilder private var root: some View {
        switch flow.phase {
        case .checkInternet, .checkUpdate, .downloading, .checkFailed:
            UpdateCheckView(palette: p, phase: flow.phase, client: flow.client) { flow.retry() }
        case .noInternet:
            NoInternetView(palette: p) { flow.retry() }
        case .connectToCar:
            ZStack {
                p.bg.ignoresSafeArea()
                ConnectView()
            }
            .onAppear { conn.start(); status.start(); tryCarConnected() }  // fw may already be known
            .onChange(of: status.online) { _ in tryCarConnected() }
            .onChange(of: status.fw) { _ in tryCarConnected() }
        case .updateRequired:
            FirmwareView(palette: p, forced: true, onDone: { flow.updateFinished() }, status: status)
                .onAppear { conn.start(); status.start() }
        case .drive:
            ZStack {
                DriveView(conn: conn, status: status)
                if !status.online { ConnectView() }
            }
        }
    }

    private func tryCarConnected() {
        // fw is set only by a successful /status bootstrap → it's a reliable "car reached" signal
        // and (unlike `online`) isn't cleared by the telemetry-freshness timer.
        if status.fw != nil { flow.carConnected(carFw: status.fw) }
    }
}
