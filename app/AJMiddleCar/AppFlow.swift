import Foundation

/// The launch gate: internet → fetch/cache the firmware → wait for the car → force the update if
/// the car is behind → hand over.
///
/// It no longer has a `.drive` state. Driving is not a phase the gate can enter and latch; it is
/// what `CarLink` says is true right now, so a car that goes away mid-session is handled by the
/// screen rather than by a state machine that has already decided.
@MainActor
final class AppFlow: ObservableObject {
    enum Phase: Equatable {
        case checkInternet, noInternet, checkUpdate, checkFailed, downloading
        /// The gate has passed; the car has not identified itself yet. What is on screen while
        /// this lasts comes from `CarLink` — searching, wrong car, no Wi-Fi, denied.
        case awaitingCar
        case updateRequired
        /// The gate is satisfied. `CarLink` decides whether that means the drive screen.
        case ready

        /// The phases whose screens open the link — the same set `root` switches on. The
        /// scene handler restarts the link on `.active` and must not open a session behind
        /// a gate screen that says there is nothing to talk to.
        var opensLink: Bool {
            switch self {
            case .updateRequired, .awaitingCar, .ready: return true
            case .checkInternet, .noInternet, .checkUpdate, .checkFailed, .downloading: return false
            }
        }
    }

    @Published var phase: Phase = .checkInternet
    @Published var latestTag: String?
    let client = UpdateClient()

    /// Run the pre-connect gate (internet probe → latest release → download if needed).
    func startupCheck() async {
        phase = .checkInternet
        guard await UpdateClient.internetReachable() else {
            phase = offlineFallback(or: .noInternet)
            return
        }
        phase = .checkUpdate
        guard let rel = await client.latestRelease() else {
            phase = offlineFallback(or: .checkFailed)
            return
        }
        latestTag = rel.tag
        let latestBuild = UpdateClient.buildNumber(rel.tag)
        if UpdateClient.needsDownload(latestBuild: latestBuild,
                                      cachedBuild: UpdateClient.cachedBuild,
                                      hasCachedFile: UpdateClient.hasCachedFile) {
            phase = .downloading
            let t0 = Date()
            guard await client.download(rel.assetURL) != nil else {
                // The two failure paths above fall back to the cache; a failed download of a
                // NEWER release must not strand a phone that still holds the previous one.
                phase = offlineFallback(or: .checkFailed)
                return
            }
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
            if let b = latestBuild { UpdateClient.recordCache(build: b, tag: rel.tag) }
        }
        phase = .awaitingCar
    }

    /// GitHub unreachable or unusable: a cached image is enough to drive — and enough to
    /// force with. Seeding `latestTag` from the cache is what keeps the forced gate armed
    /// offline (decision 4a); without it `mustUpdate` compared against nil and every car,
    /// pre-versioning ones included, drove unforced whenever the launch had no internet.
    private func offlineFallback(or failure: Phase) -> Phase {
        guard GateRule.canProceedOffline(hasCachedFile: UpdateClient.hasCachedFile,
                                         cachedBuild: UpdateClient.cachedBuild) else {
            return failure
        }
        latestTag = GateRule.offlineLatestTag(cachedTag: UpdateClient.cachedTag,
                                              hasCachedFile: UpdateClient.hasCachedFile,
                                              cachedBuild: UpdateClient.cachedBuild)
        return .awaitingCar
    }

    /// The car said who it is, in its hello reply. Re-evaluated every time, not once: a car that
    /// reboots into a different build after an OTA is the same question asked again.
    ///
    /// A nil `fw` is "we have not met the car yet", which is not the same as "the car is behind"
    /// — forcing an update against a car that never answered would be a screen with nothing to
    /// flash.
    ///
    /// `.updateRequired` is in the set so the gate can CLEAR: a car that reboots into the
    /// required build must release the forced screen even if FirmwareView's own confirmation
    /// window missed the reconnect (decision 4c).
    func carIdentified(fw: String?) {
        guard let fw else { return }
        guard phase == .awaitingCar || phase == .ready || phase == .updateRequired else { return }
        let next: Phase = UpdateClient.mustUpdate(carFw: fw, latestTag: latestTag) ? .updateRequired : .ready
        // Only on a real change: this is re-asked on every telemetry frame, and `@Published`
        // emits on assignment whether or not the value moved — five root-tree invalidations a
        // second for a phase that has not changed since launch.
        if phase != next { phase = next }
    }

    /// Forced FirmwareView signals completion.
    func updateFinished() { if phase == .updateRequired { phase = .ready } }

    func retry() { Task { await startupCheck() } }
}
