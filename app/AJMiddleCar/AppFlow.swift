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
        /// Before the first `/status` has come back at all — not yet known whether a dongle is
        /// even attached. Distinct from `.dongleAbsent` (which is a definite "nothing answered"
        /// after actually asking): the two would otherwise flash "plug it in" at every cold
        /// launch, dongle attached or not, for as long as the first request takes.
        case checkDongle
        /// Nothing answered `/status` — `DongleLink.next(reply: .silent, ...)`'s own step, not
        /// an error the flow invents separately.
        case dongleAbsent
        /// Something answered at the dongle's address and it was not usable — an HTTP error, a
        /// truncated stream, a body that would not decode. Deliberately not `.dongleAbsent`:
        /// the one instruction that screen gives is the one thing already done.
        case dongleFault
        /// Local-network access is denied, so no request this app makes ever leaves the phone.
        /// Renders the same screen `CarLink` shows for it later, with the same button — the
        /// gate had no way to say it at all before, and said "plug in an adapter" instead.
        case dongleDenied
        /// Something is answering at the dongle's address and it is not our dongle
        /// (`DongleStep.wrongDongle`). Carries what it called itself, so the screen can name it.
        case dongleWrong(device: String)
        /// The dongle's own firmware is behind the latest release; downloading and flashing it,
        /// before anything else in this sequence touches the car. Reused for the short reboot
        /// that follows a successful flash — see `performDongleUpdate()`.
        case dongleUpdating
        /// `performDongleUpdate()` failed `maxDongleUpdateAttempts` times in a row (no internet
        /// and no usable cache, or the upload itself kept failing) — `dongleGate()` stops
        /// retrying automatically and waits for `retryDongleUpdate()` instead of re-downloading
        /// from GitHub every poll forever.
        case dongleUpdateFailed
        /// The dongle's bootloader reverted its last update. Standing until `skipDongleRollback()`
        /// is called — the rollback flag itself does not clear until a LATER OTA to that slot
        /// succeeds, so without an acknowledgement path this phase would never release.
        case dongleRolledBack
        /// The dongle is current and pointed at the car's own network. Either its credentials
        /// are being sent for the first time (including a re-point, if it was pointed at some
        /// other network), or the radio is working through its own join budget (`joining`, or a
        /// state this build does not recognise) — both render the same "connecting" screen; see
        /// `DongleLink.DongleStep.sendCredentials`/`.waiting`.
        case dongleConfiguring
        /// The dongle will not get any further on its own: the join budget ran out (`failed`),
        /// or its state machine never left `idle` — see `DongleStep.retryJoin`. The credentials
        /// are already stored; `dongleGate()` asks the radio to try again.
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
            case .checkDongle, .dongleAbsent, .dongleFault, .dongleDenied, .dongleWrong,
                 .dongleUpdating, .dongleUpdateFailed, .dongleRolledBack,
                 .dongleConfiguring, .dongleJoinFailed,
                 .checkInternet, .noInternet, .checkUpdate, .checkFailed, .downloading: return false
            }
        }
    }

    @Published var phase: Phase = .checkDongle
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

    /// A judgement, not a measurement, matching the same shape as the firmware's own
    /// `WIFI_JOIN_ATTEMPTS`: enough that a single flaky download or upload does not give up
    /// early, few enough that a phone with no usable internet and no cache is told so within a
    /// few tries rather than re-fetching from GitHub on every poll forever.
    private static let maxDongleUpdateAttempts = 3

    /// Guards against a second `startupCheck()` running while one is already in flight — a
    /// second tap on a retry button whose screen has not yet updated `phase` (`dongleGate()`'s
    /// first act is an `await` on `/status`, up to its timeout, before it writes anything) would
    /// otherwise spawn a second poll loop issuing requests at the dongle's fixed address
    /// alongside the first, which is exactly what `DongleClient` asks callers not to do — and if
    /// the first loop has already handed off to a live drive session, the second would still be
    /// out there writing `phase` out from under it on its own next poll.
    private var gateRunning = false

    /// Set by `skipDongleRollback()`. See `Phase.dongleRolledBack`'s doc for why this has to
    /// exist at all: the bootloader's rollback flag does not clear on its own.
    private var rollbackAcknowledged = false
    private var dongleUpdateAttempts = 0
    private var dongleUpdateGaveUp = false

    /// Entry point. Re-entrant calls while a run is already in flight are ignored — see
    /// `gateRunning`'s own doc.
    func startupCheck() async {
        guard !gateRunning else { return }
        gateRunning = true
        defer { gateRunning = false }
        #if targetEnvironment(simulator)
        // On the simulator there has never been a dongle and the spec is explicit that this
        // plan does not build one to stand in for it — the simulator keeps talking to
        // `tools/mock_car` directly, exactly as it did before this sequence existed — so the
        // whole dongle half is skipped there and only on there.
        await carGate()
        #else
        if CarHost.direct {
            // The bench escape hatch (`-carHost`, see `CarHost.direct`): the car is addressed
            // directly over the phone's own Wi-Fi, so there is no dongle in this path at all —
            // nothing to find, nothing to update, nobody to hand credentials to. The gate must
            // not run here. It would poll `DongleContract.host`, which nothing on this path
            // answers, and latch `.dongleAbsent` — whose `opensLink` is false — so the link
            // would never open and the argument would address a car nothing ever talks to.
            // That is the hatch disabled by the very gate it exists to bypass, and the hatch is
            // the only instrument this branch has for telling an app fault from a dongle fault.
            await carGate()
            return
        }
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
            let reply = await readStatus()
            if case .status = reply, !checkedForUpdate {
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
            switch DongleLink.next(reply: reply, latestTag: tag, expectedSSID: CarContract.ssid,
                                   rollbackAcknowledged: rollbackAcknowledged) {
            case .plugIn:
                phase = .dongleAbsent
            case .faulty:
                phase = .dongleFault
            case .accessDenied:
                phase = .dongleDenied
            case .wrongDongle(let device):
                phase = .dongleWrong(device: device)
            case .rolledBack:
                phase = .dongleRolledBack
            case .updating:
                if dongleUpdateGaveUp {
                    phase = .dongleUpdateFailed
                } else {
                    phase = .dongleUpdating
                    if await performDongleUpdate(release: release) {
                        dongleUpdateAttempts = 0
                    } else {
                        dongleUpdateAttempts += 1
                        if dongleUpdateAttempts >= Self.maxDongleUpdateAttempts {
                            dongleUpdateGaveUp = true
                            phase = .dongleUpdateFailed
                        }
                    }
                }
            case .sendCredentials:
                phase = .dongleConfiguring
                // No credential state lives here or in DongleClient — CarContract's are opaque
                // constants, read and handed over, never logged or shown (see DongleClient's own
                // doc for why `join`/`retryJoin` are shaped this way).
                await askDongleToJoin(retry: false)
            case .waiting:
                phase = .dongleConfiguring
            case .retryJoin:
                phase = .dongleJoinFailed
                await askDongleToJoin(retry: true)
            case .readyForCar:
                await carGate()
                return
            }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }

    /// One `/status` read, classified rather than collapsed into a bare optional, and logged
    /// either way.
    ///
    /// `try?` here was the wrong trade on this branch specifically. Its whole justification is
    /// that the app half and the dongle half fail in the same place with the same symptom, so a
    /// rejected SSID (400), "stored, but the radio refused the join" (500), an undecodable body
    /// and no cable at all were four different faults with four different fixes — flattened
    /// into one `nil`, rendered as one screen telling the user to plug in a dongle that is
    /// plugged in and answering, with nothing written to the log either.
    /// `UpdateClient.upload` already logs its own failures for exactly this reason.
    private func readStatus() async -> DongleReply {
        do {
            return .status(try await dongle.status())
        } catch {
            let reply = DongleReply.of(error)
            print("dongle \(DongleContract.statusPath) failed: \(Self.describe(error))")
            return reply
        }
    }

    /// One POST asking the dongle to join the car's network — `join` the first time, `retryJoin`
    /// afterwards (`DongleClient` keeps them apart on purpose; see `retryJoin`'s doc). Failures
    /// are logged, never swallowed: a 400 means the dongle rejected the credentials outright and
    /// a 500 means it stored them and the radio refused, which are the same screen but very
    /// different bench sessions.
    private func askDongleToJoin(retry: Bool) async {
        do {
            if retry {
                try await dongle.retryJoin(ssid: CarContract.ssid, password: CarContract.password)
            } else {
                try await dongle.join(ssid: CarContract.ssid, password: CarContract.password)
            }
        } catch {
            // `logDescription` names the failure's shape and nothing else — no body, and
            // nothing of what was sent. The credentials never reach a log.
            print("dongle \(DongleContract.netPath) (\(retry ? "retry" : "configure")) failed: "
                  + Self.describe(error))
        }
    }

    /// `CarError.logDescription` when it is one — the same vocabulary `UpdateClient.upload`
    /// logs — and a plain description otherwise, which is how a `DecodingError` from a body
    /// this build could not read says which key it choked on.
    private static func describe(_ error: Error) -> String {
        (error as? CarError)?.logDescription ?? String(describing: error)
    }

    /// The user chose to proceed on the dongle's current, reverted firmware rather than being
    /// stuck on `.dongleRolledBack` forever (the car's own forced-update gate keeps the same
    /// escape hatch — `FirmwareView`'s skip button). Read by `dongleGate()`'s very next poll,
    /// which is at most `donglePollInterval` away.
    func skipDongleRollback() { rollbackAcknowledged = true }

    /// The user asked `dongleGate()` to try updating the dongle again after it gave up —
    /// `maxDongleUpdateAttempts` consecutive failures with no progress. Clears both counters so
    /// the very next `.updating` step gets a fresh budget.
    func retryDongleUpdate() {
        dongleUpdateGaveUp = false
        dongleUpdateAttempts = 0
    }

    /// Download (or reuse the cached image) and flash the dongle's own firmware, then give it a
    /// moment to reboot before `dongleGate()`'s loop resumes polling normally. Returns whether
    /// the upload was actually accepted; `dongleGate()` uses that to bound retries instead of
    /// silently re-downloading from GitHub on every poll forever.
    ///
    /// The decision of what to flash — download `release`'s asset, reuse what is already
    /// cached, or give up because neither is available — is `UpdateRules.flashPlan(for:...)`,
    /// pure and host-tested; this function performs only the network/disk work that plan names,
    /// and always for the device the plan itself carries (see `flashPlan`'s own doc for why that
    /// is the whole point of it returning a value rather than a bare URL).
    private func performDongleUpdate(release: UpdateClient.Release?) async -> Bool {
        let plan = UpdateRules.flashPlan(for: .dongle,
                                         release: release.map { (tag: $0.tag, assetURL: $0.assetURL) },
                                         cachedBuild: UpdateClient.cachedBuild(for: .dongle),
                                         hasCachedFile: UpdateClient.hasCachedFile(for: .dongle))
        let binURL: URL?
        switch plan {
        case .download(let device, let url, let recordBuild, let tag):
            let recordAs = recordBuild.map { (build: $0, tag: tag) }
            binURL = await client.download(url, recordAs: recordAs, device: device)
        case .useCache(let device):
            binURL = UpdateClient.cachedBinURL(for: device)
        case .unavailable:
            binURL = nil
        }
        guard let binURL, let data = try? Data(contentsOf: binURL) else { return false }
        do {
            try await dongle.uploadFirmware(data) { _ in }
        } catch {
            // Not swallowed into "success": a failed upload must count against the attempt
            // budget above, and must not earn the reboot grace below — nothing rebooted.
            return false
        }
        try? await Task.sleep(for: Self.dongleRebootGrace)
        return true
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
