import Foundation

/// Drives the launch gate: internet → fetch/cache firmware → connect to car → force-update if stale → drive.
@MainActor
final class AppFlow: ObservableObject {
    enum Phase: Equatable {
        case checkInternet, noInternet, checkUpdate, checkFailed, downloading, connectToCar, updateRequired, drive
    }
    @Published var phase: Phase = .checkInternet
    @Published var latestTag: String?
    let client = UpdateClient()

    /// Run the pre-connect gate (internet probe → latest release → download if needed).
    func startupCheck() async {
        phase = .checkInternet
        guard await UpdateClient.internetReachable() else { phase = .noInternet; return }
        phase = .checkUpdate
        guard let rel = await client.latestRelease() else { phase = .checkFailed; return }
        latestTag = rel.tag
        let latestBuild = UpdateClient.buildNumber(rel.tag)
        if UpdateClient.needsDownload(latestBuild: latestBuild,
                                      cachedBuild: UpdateClient.cachedBuild,
                                      hasCachedFile: UpdateClient.hasCachedFile) {
            phase = .downloading
            let t0 = Date()
            guard await client.download(rel.assetURL) != nil else { phase = .checkFailed; return }
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
            if let b = latestBuild { UpdateClient.recordCache(build: b, tag: rel.tag) }
        }
        phase = .connectToCar
    }

    /// Called once the car is reachable and its fw is known (on the connectToCar phase).
    func carConnected(carFw: String?) {
        guard phase == .connectToCar else { return }
        phase = UpdateClient.mustUpdate(carFw: carFw, latestTag: latestTag) ? .updateRequired : .drive
    }

    /// Forced FirmwareView signals completion.
    func updateFinished() { if phase == .updateRequired { phase = .drive } }

    func retry() { Task { await startupCheck() } }
}
