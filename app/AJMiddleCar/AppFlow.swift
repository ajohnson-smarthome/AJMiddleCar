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
        /// Step 2. Something answered at the adapter's address and is being looked over. Brief
        /// by nature — the reply is already in hand when this is set — which is exactly why
        /// `PhasePacer` exists: without it this step would never be legible.
        case dongleChecking
        /// Step 3. Asking GitHub whether the adapter's own firmware is current. Had no phase at
        /// all before, so this wait happened behind whatever screen preceded it.
        case dongleUpdateCheck
        /// Step 4. The adapter's radio is scanning and has not seen the car's network yet —
        /// `DongleStep.searchingCar`. Distinct from `.dongleConfiguring`, which is the
        /// association that follows: this one usually means the car is switched off.
        case carFinding
        /// The adapter's newest release could not be established — no internet, or a release
        /// with no build number in it. A hold, not a failure: `dongleGate()` keeps asking, so
        /// this clears itself the moment the network returns. That is also why it is not
        /// `.noInternet`, whose screen offers a Retry: while this loop is running `retry()` is
        /// refused by `gateRunning`, and a button that does nothing is worse than no button.
        case dongleOffline
        /// A release exists and carries no image for the adapter, so its version cannot be
        /// established and the gate will not hand over. Not the user's to fix — only publishing
        /// a release with the adapter's image clears it — which is why the screen says that
        /// instead of blaming the network.
        case dongleNoRelease(tag: String)
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
        /// The dongle's bootloader reverted its last update. Standing until the user answers —
        /// the rollback flag itself does not clear until a LATER OTA to that slot succeeds, so
        /// without an answer this phase would never release. Two answers, both needed:
        /// `recheckDongleRollback()` asks whether a newer release exists yet, and it is the only
        /// answer there is: the option to drive on the reverted firmware was removed with the
        /// rest of the escapes. The app is the dongle's only OTA path, so a release that keeps
        /// rolling back holds here until a newer one ships.
        case dongleRolledBack
        /// The dongle is current and pointed at the car's own network. Either its credentials
        /// are being sent for the first time (including a re-point, if it was pointed at some
        /// other network), or the radio is working through its own join budget (`joining`, or a
        /// state this build does not recognise) — both render the same "connecting" screen; see
        /// `DongleLink.DongleStep.sendCredentials`/`.waiting`.
        case dongleConfiguring
        /// The dongle will not get any further on its own: the join budget ran out (`failed`),
        /// or its state machine never left `idle` — see `DongleStep.retryJoin`. The credentials
        /// are already stored; `dongleGate()` asks the radio to try again, at most
        /// `maxDongleJoinAttempts` times, and then holds here with a button. The spec is
        /// explicit that this state is "reached and held rather than retried forever… A radio
        /// that hunts for an absent car indefinitely is drawing the phone's battery for
        /// nothing", and that after a few attempts the app says the car is not found and offers
        /// a Retry.
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
            case .checkDongle, .dongleAbsent, .dongleChecking, .dongleUpdateCheck, .carFinding,
                 .dongleOffline, .dongleNoRelease,
                 .dongleFault, .dongleDenied, .dongleWrong,
                 .dongleUpdating, .dongleUpdateFailed, .dongleRolledBack,
                 .dongleConfiguring, .dongleJoinFailed,
                 .checkInternet, .noInternet, .checkUpdate, .checkFailed, .downloading: return false
            }
        }
    }

    @Published var phase: Phase = .checkDongle
    /// What is actually on screen. Lags `phase` by at most `PhasePacer.minVisible` per step, so
    /// a launch whose steps resolve instantly is a readable sequence rather than one frame of
    /// strobing. Everything that renders reads this; everything that decides reads `phase`.
    @Published private(set) var shown: Phase = .checkDongle
    private var shownAt = Date()
    private var queued: [Phase] = []
    private var pacing = false
    @Published var latestTag: String?
    /// How far the adapter's own update has got, while one is running — and `nil` whenever that
    /// cannot be answered honestly. It covers the upload half only: the download half is a fetch
    /// from GitHub whose progress `UpdateClient` already publishes for its own screen, and a
    /// cache hit has no download at all. An invented figure would be worse than none, because it
    /// teaches the user not to believe the next one.
    @Published private(set) var dongleUpdateProgress: Double?
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

    /// The same shape, for the other unbounded loop: how many times `dongleGate()` will POST
    /// the car's network at the dongle — the first configure and every retry together — before
    /// it stops and waits for `retryDongleJoin()`. Nothing here was bounded at all: `.retryJoin`
    /// re-POSTed on every poll forever, which is precisely the radio the spec says must not
    /// hunt for an absent car indefinitely. Three is the update path's number and roughly the
    /// firmware's own `WIFI_JOIN_ATTEMPTS` shape; each attempt costs a full join budget on the
    /// dongle's side, so this is minutes of honest trying, not seconds.
    private static let maxDongleJoinAttempts = 3

    /// Guards against a second `startupCheck()` running while one is already in flight — a
    /// second tap on a retry button whose screen has not yet updated `phase` (`dongleGate()`'s
    /// first act is an `await` on `/status`, up to its timeout, before it writes anything) would
    /// otherwise spawn a second poll loop issuing requests at the dongle's fixed address
    /// alongside the first, which is exactly what `DongleClient` asks callers not to do — and if
    /// the first loop has already handed off to a live drive session, the second would still be
    /// out there writing `phase` out from under it on its own next poll.
    private var gateRunning = false

    /// Set by `recheckDongleRollback()`. See `Phase.dongleRolledBack`
    /// and `RollbackChoice` for why this has to exist at all: the bootloader's rollback flag
    /// does not clear on its own, and the app is the dongle's only OTA path.
    private var rollbackChoice: RollbackChoice = .unanswered

    /// The dongle's release and the tag `DongleLink` compares against — fetched once, lazily,
    /// the first time `/status` answers, and held on the flow rather than inside `dongleGate()`
    /// so `recheckDongleRollback()` can reopen the fetch (that is what "check again" means).
    private var dongleRelease: UpdateClient.Release?
    private var dongleLatestTag: String?
    /// Whether `/status` has ever answered this launch — the trigger for step 2, and nothing
    /// else.
    private var sawDongle = false
    private var dongleUpdateAttempts = 0
    private var dongleUpdateGaveUp = false
    private var dongleJoinAttempts = 0
    private var dongleJoinGaveUp = false
    /// The last `/status` failure written to the log — see `readStatus()` for why it is
    /// remembered at all.
    private var lastStatusFailure: String?

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
        await carGate()
        #endif
    }

    /// The dongle's interface came back after going away.
    ///
    /// Unplugging mid-drive is handled correctly all the way down — `CarPath` goes unsatisfied,
    /// `CarLink` says the wire is gone, the screen says so — but `dongleGate()` returned for
    /// good when it handed over, so nothing is left watching the dongle. If the dongle comes
    /// back having failed to rejoin the car (the car was switched off in the meantime, so its
    /// own join budget ran out while nobody was asking), it sits in `net.state: failed`
    /// answering nobody, and `CarLink` radars indefinitely with no path back to the join logic.
    /// This is that path, and it is the only hole in an otherwise complete unplug story.
    ///
    /// Only the dongle half re-runs. The car's own gate already answered this session and
    /// `latestTag` is still held, so re-running it would re-probe GitHub and could strand a
    /// live session on `.noInternet` over a cable that was out for two seconds. Handing back to
    /// `.awaitingCar` is enough: `carIdentified` restores `.ready`/`.updateRequired` on the
    /// next hello, which is where the phase was before the wire went.
    func dongleReturned() async {
        #if !targetEnvironment(simulator)
        // No dongle in the escape hatch's path at all, and nothing to re-ask if the gate never
        // handed over in the first place — a flap during the launch gate is that gate's own
        // business, and `gateRunning` keeps two loops from polling the same address.
        guard !CarHost.direct, !gateRunning else { return }
        guard phase == .awaitingCar || phase == .ready || phase == .updateRequired else { return }
        gateRunning = true
        defer { gateRunning = false }
        await dongleGate()
        setPhase(.awaitingCar)
        #endif
    }

    /// Poll the dongle until it reports `.readyForCar`, acting on whatever `DongleLink` says is
    /// next at each step, and return. What follows is the caller's: `startupCheck()` hands over
    /// to `carGate()`, and `dongleReturned()` — a re-entry after the wire came back — does not,
    /// because the car's gate has already answered. One release tags both images identically
    /// (`UpdateRules.Device`), so the tag fetched here for the dongle's own comparison is a
    /// separate call from the one `carGate()` makes for the car's — decoupled on purpose, so
    /// neither device's gate reads a tag fetched for the other's asset URL.
    private func dongleGate() async {
        // Fetched once, lazily, the first time `/status` actually answers — not up front. The
        // spec's own order is "check whether a dongle is there... if it is, check for a newer
        // version": fetching GitHub before the first presence check would make a phone with
        // nothing plugged in wait on a network round trip just to be told to plug something in.
        while true {
            let reply = await readStatus()
            // Step 2, once: something is there and is being looked over. Guarded, because this
            // loop re-reads /status forever and must not walk the ladder backwards on every poll.
            if case .status = reply, !sawDongle {
                sawDongle = true
                setPhase(.dongleChecking)
            }
            // Step 3, and a gate rather than a formality: the adapter's newest release must be
            // established before anything is decided about it. Retried on every poll until it
            // is — a launch that could not reach GitHub must not proceed on the assumption that
            // nothing has changed, which is exactly what it used to do.
            if case .status = reply, dongleLatestTag == nil {
                // Announce the check only when not already holding on a failure of it. This loop
                // re-asks every poll, and re-announcing each time made "checking" and the hold
                // alternate — with `PhasePacer` guaranteeing each screen its 400 ms, that is a
                // strobe rather than a sequence, which is exactly the complaint this whole
                // redesign began from.
                var holding = phase == .dongleOffline
                if case .dongleNoRelease = phase { holding = true }
                if !holding {
                    setPhase(.dongleUpdateCheck)      // step 3
                }
                switch await client.latestReleaseLookup(for: .dongle) {
                case .found(let rel):
                    dongleRelease = rel
                    // The tag is only adopted once it can be compared against. Setting it first
                    // and validating after left an unusable tag in place, and the next poll then
                    // skipped this whole block and drove on it.
                    guard GateRule.canVerify(latestBuild: UpdateClient.buildNumber(rel.tag)) else {
                        setPhase(.dongleNoRelease(tag: rel.tag))
                        try? await Task.sleep(for: Self.donglePollInterval)
                        continue
                    }
                    dongleLatestTag = rel.tag
                case .noImage(let tag):
                    setPhase(.dongleNoRelease(tag: tag))
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                case .unreachable:
                    setPhase(.dongleOffline)
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
            switch DongleLink.next(reply: reply, latestTag: dongleLatestTag,
                                   expectedSSID: CarContract.ssid, rollback: rollbackChoice) {
            case .plugIn:
                setPhase(.dongleAbsent)
            case .faulty:
                setPhase(.dongleFault)
            case .accessDenied:
                setPhase(.dongleDenied)
            case .wrongDongle(let device):
                setPhase(.dongleWrong(device: device))
            case .rolledBack:
                // One look per ask: a recheck that found nothing newer is spent here, so the
                // screen comes back with both its buttons instead of re-asking GitHub on every
                // poll from a permission the user gave once.
                consumeRollbackRecheck()
                setPhase(.dongleRolledBack)
            case .updating:
                consumeRollbackRecheck()
                if dongleUpdateGaveUp {
                    setPhase(.dongleUpdateFailed)
                } else {
                    setPhase(.dongleUpdating)
                    if await performDongleUpdate(release: dongleRelease) {
                        dongleUpdateAttempts = 0
                    } else {
                        dongleUpdateAttempts += 1
                        if dongleUpdateAttempts >= Self.maxDongleUpdateAttempts {
                            dongleUpdateGaveUp = true
                            setPhase(.dongleUpdateFailed)
                        }
                    }
                }
            case .sendCredentials:
                // Once the budget is spent this is the same dead end `.retryJoin` reaches, and
                // it says so: a dongle that keeps reporting a network other than the car's
                // after being told the car's is not "configuring", it is failing to configure.
                setPhase(dongleJoinGaveUp ? .dongleJoinFailed : .dongleConfiguring)
                // No credential state lives here or in DongleClient — CarContract's are opaque
                // constants, read and handed over, never logged or shown (see DongleClient's own
                // doc for why `join`/`retryJoin` are shaped this way).
                await askDongleToJoin(retry: false)
            case .searchingCar:
                setPhase(.carFinding)
            case .waiting:
                setPhase(.dongleConfiguring)
            case .retryJoin:
                setPhase(.dongleJoinFailed)
                await askDongleToJoin(retry: true)
            case .readyForCar:
                // The join worked, so the budget that got here is spent on nothing: reset it,
                // for this session and for anyone who re-enters this loop later.
                dongleJoinAttempts = 0
                dongleJoinGaveUp = false
                return
            }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }

    /// Write `phase` only when it actually moves.
    ///
    /// `dongleGate()` re-decides the phase on every poll and assigns it unconditionally, and
    /// `@Published` emits on assignment whether or not the value changed — so an unplugged
    /// phone re-invalidated the whole root tree at 0.67 Hz, indefinitely, to redraw the same
    /// screen. `carIdentified` guards against exactly this three lines from here, for exactly
    /// the same reason, at a higher rate.
    private func setPhase(_ next: Phase) {
        guard phase != next else { return }
        phase = next
        queued.append(next)
        guard !pacing else { return }
        pacing = true
        // Strong capture on purpose: `AppFlow` is a `@StateObject` that outlives every launch
        // this drains, and a weak one would only add a way for the queue to stop halfway with
        // a stale screen on display.
        Task { @MainActor in
            while !self.queued.isEmpty {
                let wait = PhasePacer.wait(shownAt: self.shownAt.timeIntervalSinceReferenceDate,
                                           now: Date().timeIntervalSinceReferenceDate)
                if wait > 0 { try? await Task.sleep(for: .seconds(wait)) }
                guard !self.queued.isEmpty else { break }
                self.shown = self.queued.removeFirst()
                self.shownAt = Date()
            }
            self.pacing = false
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
            let status = try await dongle.status()
            lastStatusFailure = nil
            return .status(status)
        } catch {
            // Once per distinct failure, not once per poll: this loop runs at
            // `donglePollInterval` for as long as the cable is out, and a log that repeats the
            // same line forever is as unreadable as the silence it replaced. A CHANGE of
            // failure is the interesting event — "no dongle" becoming "http 500" is the dongle
            // arriving and refusing, which is exactly what a bench operator is watching for.
            let what = Self.describe(error)
            if what != lastStatusFailure {
                lastStatusFailure = what
                print("dongle \(DongleContract.statusPath) failed: \(what)")
            }
            return DongleReply.of(error)
        }
    }

    /// One POST asking the dongle to join the car's network — `join` the first time, `retryJoin`
    /// afterwards (`DongleClient` keeps them apart on purpose; see `retryJoin`'s doc). Failures
    /// are logged, never swallowed: a 400 means the dongle rejected the credentials outright and
    /// a 500 means it stored them and the radio refused, which are the same screen but very
    /// different bench sessions.
    private func askDongleToJoin(retry: Bool) async {
        // Bounded, and the bound covers BOTH kinds of ask: a dongle that never stores what it
        // is told loops through `.sendCredentials` exactly as tirelessly as a radio that cannot
        // reach the car loops through `.retryJoin`, and neither may run forever.
        guard !dongleJoinGaveUp else { return }
        dongleJoinAttempts += 1
        if dongleJoinAttempts >= Self.maxDongleJoinAttempts { dongleJoinGaveUp = true }
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

    /// The user asked whether a newer release exists yet — `FirmwareView`'s rolled-back car
    /// screen keeps the same offer beside its skip. Two halves, both required: re-open the
    /// release fetch (a tag fetched before the rollback screen appeared is exactly the tag that
    /// cannot help), and record what was on offer at the time so `DongleLink` can tell a
    /// genuinely newer image from the one that just rolled back.
    func recheckDongleRollback() {
        rollbackChoice = .recheck(from: dongleLatestTag)
        // Clearing the tag is what makes the next poll re-ask GitHub: the fetch is guarded on
        // `dongleLatestTag == nil`, so this is the recheck actually happening.
        dongleLatestTag = nil
    }

    /// A recheck is one look, not a standing permission — spent as soon as `DongleLink` has
    /// answered with it, whichever way it answered.
    private func consumeRollbackRecheck() {
        if case .recheck = rollbackChoice { rollbackChoice = .unanswered }
    }

    /// The user asked for another join attempt from the join-failed screen — either after the
    /// budget above ran out, or just to stop waiting for the next poll. Both are the same
    /// request: a fresh budget, spent from the next `.sendCredentials`/`.retryJoin` step.
    func retryDongleJoin() {
        dongleJoinGaveUp = false
        dongleJoinAttempts = 0
    }

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
        dongleUpdateProgress = 0
        defer { dongleUpdateProgress = nil }
        do {
            try await dongle.uploadFirmware(data) { fraction in
                Task { @MainActor in self.dongleUpdateProgress = fraction }
            }
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
        setPhase(.checkInternet)
        // No fallback any more. A cached image says what this phone downloaded once, not what
        // the newest release is now, and letting it stand in for a check was the whole leak.
        guard await UpdateClient.internetReachable() else {
            setPhase(.noInternet)
            return
        }
        setPhase(.checkUpdate)
        // The car's half tells the two apart too, though only one of them has ever fired: the
        // car's image has been in every release. Both land on `.checkFailed`, which carries a
        // Retry — this gate returns rather than looping, so a button is the way back.
        let lookup = await client.latestReleaseLookup()
        guard case .found(let rel) = lookup else {
            setPhase(.checkFailed)
            return
        }
        latestTag = rel.tag
        let latestBuild = UpdateClient.buildNumber(rel.tag)
        // A release whose tag carries no build number is not a verification either: there is
        // nothing to compare against, and "could not tell" must never read as "current".
        guard GateRule.canVerify(latestBuild: latestBuild) else {
            setPhase(.checkFailed)
            return
        }
        if UpdateClient.needsDownload(latestBuild: latestBuild,
                                      cachedBuild: UpdateClient.cachedBuild,
                                      hasCachedFile: UpdateClient.hasCachedFile) {
            setPhase(.downloading)
            let t0 = Date()
            let recordAs = latestBuild.map { (build: $0, tag: rel.tag) }
            guard await client.download(rel.assetURL, recordAs: recordAs) != nil else {
                // The two failure paths above fall back to the cache; a failed download of a
                // NEWER release must not strand a phone that still holds the previous one.
                setPhase(.checkFailed)
                return
            }
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
        }
        setPhase(.awaitingCar)
    }

    /// GitHub unreachable or unusable: a cached image is enough to drive — and enough to
    /// force with. Seeding `latestTag` from the cache is what keeps the forced gate armed
    /// offline (decision 4a); without it `mustUpdate` compared against nil and every car,
    /// pre-versioning ones included, drove unforced whenever the launch had no internet.
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
        let next: Phase = GateRule.mayDrive(deviceBuild: UpdateClient.buildNumber(fw),
                                           latestBuild: UpdateClient.buildNumber(latestTag))
            ? .ready : .updateRequired
        // `setPhase` already writes only on a real change, which matters here more than
        // anywhere: this is re-asked on every telemetry frame, and `@Published` emits on
        // assignment whether or not the value moved — five root-tree invalidations a second for
        // a phase that has not changed since launch.
        setPhase(next)
    }

    /// Forced FirmwareView signals completion.
    func updateFinished() { if phase == .updateRequired { setPhase(.ready) } }

    func retry() { Task { await startupCheck() } }
}
