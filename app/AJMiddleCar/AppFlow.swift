import Foundation

/// The launch gate, in the order the spec states it: is a dongle there → update the dongle if
/// it is behind → tell it which network to join if it has not been told → internet → fetch/cache
/// the car's firmware → wait for the car → force the update if the car is behind → hand over.
///
/// The dongle half and the car half are two different questions answered by two different pure
/// modules — `DongleLink` for "what does the dongle need next", `GateRule`/`UpdateRules` for the
/// car's own offline fallback and forced-update comparison — but they share one `Phase` and one
/// `startupCheck()` entry point, because from the user's seat this has always been a single gate,
/// not two gates that happen to run back to back.
///
/// It no longer has a `.drive` state. Driving is not a phase the gate can enter and latch; it is
/// what `CarLink` says is true right now, so a car that goes away mid-session is handled by the
/// screen rather than by a state machine that has already decided.
@MainActor
final class AppFlow: ObservableObject {
    enum Phase: Equatable {
        /// No dongle answered `/status` — `DongleLink.next(status: nil, ...)`'s own step, not an
        /// error the flow invents separately.
        case dongleAbsent
        /// The dongle's own firmware is behind the latest release; downloading and flashing it,
        /// before anything else in this sequence touches the car. Reused for the short reboot
        /// that follows the flash — see `performDongleUpdate()`.
        case dongleUpdating
        /// The dongle's bootloader reverted its last update. Shown once, standing, until a
        /// working image changes it — never re-offered as `.dongleUpdating` in a loop.
        case dongleRolledBack
        /// The dongle is current. Either its credentials are being sent for the first time, or
        /// it is between join attempts (`idle`/`joining`/an unrecognised state) — both render the
        /// same "connecting" screen; see `DongleLink.DongleStep.sendCredentials`/`.waiting`.
        case dongleConfiguring
        /// `net.state == .failed`: the join attempt budget ran out. The credentials are already
        /// stored; `dongleGate()` retries the join itself, on the next poll, without a button.
        case dongleJoinFailed
        case checkInternet, noInternet, checkUpdate, checkFailed, downloading
        /// The gate has passed; the car has not identified itself yet. What is on screen while
        /// this lasts comes from `CarLink` — searching, wrong car, no dongle, denied.
        case awaitingCar
        case updateRequired
        /// The gate is satisfied. `CarLink` decides whether that means the drive screen.
        case ready

        /// The phases whose screens open the link — the same set `root` switches on. The
        /// scene handler restarts the link on `.active` and must not open a session behind
        /// a gate screen that says there is nothing to talk to. Every dongle phase belongs on
        /// the `false` side of this for exactly that reason: until the dongle reports
        /// `.readyForCar`, there is no path to the car for `CarLink` to open a session over.
        var opensLink: Bool {
            switch self {
            case .updateRequired, .awaitingCar, .ready: return true
            case .dongleAbsent, .dongleUpdating, .dongleRolledBack, .dongleConfiguring, .dongleJoinFailed,
                 .checkInternet, .noInternet, .checkUpdate, .checkFailed, .downloading: return false
            }
        }
    }

    @Published var phase: Phase = .dongleAbsent
    @Published var latestTag: String?
    let client = UpdateClient()
    private let dongle = DongleClient()

    /// How often `dongleGate()` re-reads `/status` while it is not yet `.readyForCar`. A
    /// judgement, not a measurement: fast enough that "plug it in" clears within a beat of the
    /// cable actually going in, slow enough not to matter next to the requests it is pacing —
    /// `DongleClient` asks for single-flight use, and this is what keeps every step in this loop
    /// to at most one outstanding request at a time.
    private static let donglePollInterval: Duration = .milliseconds(1500)

    /// The dongle's own update "reboots it, drops the USB interface and brings it back — short,
    /// and it recovers on its own" (spec). Without this pause the very next poll below can land
    /// inside that gap, read as "nothing answered", and flash the plug-it-in screen for a beat
    /// during a reboot that was never a real disconnect.
    private static let dongleRebootGrace: Duration = .seconds(4)

    /// Entry point. On the simulator there has never been a dongle and the spec is explicit that
    /// this plan does not build one to stand in for it — the simulator keeps talking to
    /// `tools/mock_car` directly, exactly as it did before this sequence existed — so the whole
    /// dongle half is skipped there and only on there.
    func startupCheck() async {
        #if targetEnvironment(simulator)
        await carGate()
        #else
        await dongleGate()
        #endif
    }

    /// Poll the dongle until it reports `.readyForCar`, acting on whatever `DongleLink` says is
    /// next at each step, then hand over to `carGate()` — the pre-existing pre-connect gate,
    /// unchanged below. One release tags both images identically (`UpdateRules.Device`), so the
    /// tag fetched here for the dongle's own comparison is a separate call from the one
    /// `carGate()` makes for the car's — decoupled on purpose, so neither device's gate reads a
    /// tag fetched for the other's asset URL.
    private func dongleGate() async {
        // Fetched once, lazily, the first time `/status` actually answers — not up front. The
        // spec's own order is "check whether a dongle is there... if it is, check for a newer
        // version": fetching GitHub before the first presence check would make a phone with
        // nothing plugged in wait on a network round trip just to be told to plug something in.
        var release: UpdateClient.Release?
        var tag: String?
        var checkedForUpdate = false

        while true {
            let status = try? await dongle.status()
            if status != nil, !checkedForUpdate {
                checkedForUpdate = true
                release = await client.latestRelease(for: .dongle)
                // Same offline fallback `carGate()` uses below, aimed at the dongle's own cache:
                // without internet, "the last release this phone downloaded FOR THE DONGLE, while
                // that image is still on disk" is the only tag worth comparing against —
                // GateRule.canProceedOffline's own precondition (`hasCachedFile`) is exactly what
                // makes `performDongleUpdate()` below able to flash from cache when it lands in
                // `.updating` by this path.
                tag = release?.tag ?? GateRule.offlineLatestTag(
                    cachedTag: UpdateClient.cachedTag(for: .dongle),
                    hasCachedFile: UpdateClient.hasCachedFile(for: .dongle),
                    cachedBuild: UpdateClient.cachedBuild(for: .dongle))
            }
            switch DongleLink.next(status: status, latestTag: tag) {
            case .plugIn:
                phase = .dongleAbsent
            case .rolledBack:
                phase = .dongleRolledBack
            case .updating:
                phase = .dongleUpdating
                await performDongleUpdate(release: release)
            case .sendCredentials:
                phase = .dongleConfiguring
                // No credential state lives here or in DongleClient — CarContract's are opaque
                // constants, read and handed over, never logged or shown (see DongleClient's own
                // doc for why `join`/`retryJoin` are shaped this way).
                try? await dongle.join(ssid: CarContract.ssid, password: CarContract.password)
            case .waiting:
                phase = .dongleConfiguring
            case .retryJoin:
                phase = .dongleJoinFailed
                try? await dongle.retryJoin(ssid: CarContract.ssid, password: CarContract.password)
            case .readyForCar:
                await carGate()
                return
            }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }

    /// Download (or reuse the cached image) and flash the dongle's own firmware, then give it a
    /// moment to reboot before `dongleGate()`'s loop resumes polling. Goes through
    /// `UpdateRules.Device.dongle`'s own cache path end to end — `client.download(device:)` and
    /// `UpdateClient.cachedBinURL(for:)` — so a stale car image can never be found, let alone
    /// offered to `DongleClient.uploadFirmware`, under the dongle's file, or vice versa.
    private func performDongleUpdate(release: UpdateClient.Release?) async {
        let binURL: URL?
        if let release {
            let recordAs = UpdateRules.buildNumber(release.tag).map { (build: $0, tag: release.tag) }
            binURL = await client.download(release.assetURL, recordAs: recordAs, device: .dongle)
        } else if UpdateClient.hasCachedFile(for: .dongle) {
            // Reachable only when `dongleGate()`'s offline fallback supplied `tag`: that path
            // (`GateRule.offlineLatestTag`) itself requires `hasCachedFile` to return anything
            // other than nil, so landing here with `release == nil` and `.updating` chosen means
            // a previously downloaded, previously validated image is already on disk.
            binURL = UpdateClient.cachedBinURL(for: .dongle)
        } else {
            binURL = nil
        }
        guard let binURL, let data = try? Data(contentsOf: binURL) else { return }
        try? await dongle.uploadFirmware(data) { _ in }
        try? await Task.sleep(for: Self.dongleRebootGrace)
    }

    /// The pre-existing pre-connect gate for the CAR's own firmware (internet probe → latest
    /// release → download if needed), unchanged from before this task except its name — it used
    /// to be `startupCheck()` itself. Reached only once `dongleGate()` (or the simulator's
    /// bypass) says there is a car to talk to.
    private func carGate() async {
        UpdateClient.migrateCacheIfNeeded()
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
            let recordAs = latestBuild.map { (build: $0, tag: rel.tag) }
            guard await client.download(rel.assetURL, recordAs: recordAs) != nil else {
                // The two failure paths above fall back to the cache; a failed download of a
                // NEWER release must not strand a phone that still holds the previous one.
                phase = offlineFallback(or: .checkFailed)
                return
            }
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
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
