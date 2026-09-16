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
        /// Step 3. Asking GitHub for the newest release — once per launch, for both boards: one
        /// release tags both images, so the tag learned here is what the adapter is compared
        /// against now and the car after its hello. Had no phase at all before, so this wait
        /// happened behind whatever screen preceded it.
        case releaseCheck
        /// Step 4. The adapter's radio is scanning and has not seen the car's network yet —
        /// `DongleStep.searchingCar`. Distinct from `.dongleConfiguring`, which is the
        /// association that follows: this one usually means the car is switched off.
        case carFinding
        /// The newest release could not be established: GitHub did not answer. A hold, not a
        /// failure — the gate keeps asking (`fetchRelease`), so this clears itself the moment
        /// the network returns, which is why the screen has no button.
        case releaseOffline
        /// A release exists and carries no image for `device`, or no build number, so nothing
        /// can be compared against it and the gate will not proceed. Not the user's to fix —
        /// only publishing a usable release clears it — which is why the screen says that
        /// instead of blaming the network.
        case releaseMissing(tag: String, device: UpdateRules.Device)
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
        /// The screen is `FirmwareView`, the same one the car's update uses.
        case dongleUpdating
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
        /// The dongle is current and has not been told the car's network — or was told some
        /// other one — and is being told now. Its own phase because the screen for the join
        /// («Подключаю адаптер к машинке — машинка найдена») claimed a car nobody had looked
        /// for yet: at this step the adapter has not searched at all.
        case dongleSendingNet
        /// The dongle will not get any further on its own: the join budget ran out (`failed`),
        /// or its state machine never left `idle` — see `DongleStep.retryJoin`. The credentials
        /// are already on the dongle; `dongleGate()` asks the radio to try again — once, see
        /// `maxDongleJoinAttempts` — and then holds here with a button. The spec is explicit
        /// that this state is "reached and held rather than retried forever… A radio that hunts
        /// for an absent car indefinitely is drawing the phone's battery for nothing", and that
        /// after a few attempts the app says the car is not found and offers a Retry.
        ///
        /// Shown only once the asking is over. While a retry is still owed, the gate keeps
        /// `.carFinding` on screen instead: announcing "no link" for the one poll between the
        /// dongle's `failed` and the re-ask that will put it straight back into `searching` was
        /// a verdict the app itself overturned a second and a half later.
        case dongleJoinFailed
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
            case .checkDongle, .dongleAbsent, .dongleChecking, .releaseCheck, .carFinding,
                 .releaseOffline, .releaseMissing,
                 .dongleFault, .dongleDenied, .dongleWrong,
                 .dongleUpdating, .dongleRolledBack,
                 .dongleSendingNet, .dongleConfiguring, .dongleJoinFailed: return false
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
    /// The newest release's tag — one for both boards, learned once per launch by
    /// `fetchRelease` (inside `dongleGate()`, or on its own in `releaseGate()` when there is no
    /// adapter). `DongleLink.next` compares the adapter against it; `carIdentified` the car.
    /// Cleared by `recheckDongleRollback()`, which is what makes the next poll ask again.
    @Published var latestTag: String?
    let client = UpdateClient()
    /// Shared with `FirmwareView`, which now runs the adapter's update through the same screen
    /// the car's goes through — one client, so the gate and the screen are talking to the same
    /// device over the same connection.
    let dongle = DongleClient()

    /// How often `dongleGate()` re-reads `/status` while it is not yet `.readyForCar`. A
    /// judgement, not a measurement: fast enough that "plug it in" clears within a beat of the
    /// cable actually going in, slow enough not to matter next to the requests it is pacing —
    /// `DongleClient` asks for single-flight use, and this is what keeps every step in this loop
    /// to at most one outstanding request at a time.
    private static let donglePollInterval: Duration = .milliseconds(1500)

    /// How many times `dongleGate()` will POST the car's network at the dongle — the first
    /// configure and every retry together — before it stops and waits for `retryDongleJoin()`.
    /// Nothing here was bounded at all once: `.retryJoin` re-POSTed on every poll forever, which
    /// is precisely the radio the spec says must not hunt for an absent car indefinitely.
    ///
    /// ONE, not three. Each ask costs a full join budget on the dongle's side — five attempts,
    /// counted out loud on its panel as «Попытка 1 из 5» through «5 из 5» — and three asks in a
    /// row made that count run three times over, with nothing on either screen to say why the
    /// radio had changed its mind. The bound is now the one the dongle already has: the app asks
    /// once, the dongle tries five times, and then both say so and wait for a person. A car
    /// switched on after that gets its join from the Retry button, which is what the button is
    /// for.
    private static let maxDongleJoinAttempts = 1

    /// Whether `dongleGate()` has handed over this session. `retry()` — the button on the car
    /// gate's failure screens — re-runs the car gate alone while this holds: the adapter was
    /// checked, updated and joined seconds ago, and walking it through «Ищу адаптер»,
    /// «Проверяю адаптер» and GitHub again for a car-side failure told the user nothing and
    /// cost them the whole ladder. `dongleReturned()` clears it, because a dongle that went
    /// away comes back knowing nothing.
    private var dongleHandedOver = false

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

    /// Whether `/status` has ever answered this launch — the trigger for step 2, and nothing
    /// else.
    private var sawDongle = false
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
        UpdateClient.migrateCacheIfNeeded()
        // The dongle half runs wherever there is a dongle to ask: every device, and a simulator
        // launched with `-viaDongle` (CarHost) — the adapter on the Mac's USB, the simulator as
        // the phone. It learns the newest release on the way (step 3). Against the mock there
        // is no dongle and nothing stands in for one, so the release step runs on its own; the
        // car itself is met on the far side of `.awaitingCar`, in its reply to the hello.
        if CarHost.viaDongle {
            if !dongleHandedOver {
                await dongleGate()
                dongleHandedOver = true
            }
        } else {
            await releaseGate()
        }
        setPhase(.awaitingCar)
    }

    /// Step 3, one attempt: ask GitHub for the newest release and adopt its tag. The release is
    /// one for both boards — one tag, two images — so this runs once per launch, and both the
    /// adapter's comparison (`DongleLink.next`) and the car's (`carIdentified`) read the tag it
    /// leaves in `latestTag`. `device` only says which image's presence to insist on.
    ///
    /// Returns true once `latestTag` is set. Otherwise sets the holding phase — `.releaseOffline`
    /// when GitHub could not be reached, `.releaseMissing` when the release carries no image for
    /// `device` or no build number — and returns false; the caller sleeps a poll interval and
    /// asks again. Announces `.releaseCheck` only when not already holding: re-announcing on
    /// every failed poll made "checking" and the hold alternate — with `PhasePacer` guaranteeing
    /// each screen its 400 ms, that is a strobe rather than a sequence.
    private func fetchRelease(for device: UpdateRules.Device) async -> Bool {
        var holding = phase == .releaseOffline
        if case .releaseMissing = phase { holding = true }
        if !holding { setPhase(.releaseCheck) }
        switch await client.latestReleaseLookup(for: device) {
        case .found(let rel):
            // The tag is only adopted once it can be compared against. Setting it first and
            // validating after left an unusable tag in place, and the next poll then skipped
            // this whole block and drove on it.
            guard GateRule.canVerify(latestBuild: UpdateClient.buildNumber(rel.tag)) else {
                setPhase(.releaseMissing(tag: rel.tag, device: device))
                return false
            }
            latestTag = rel.tag
            return true
        case .noImage(let tag):
            setPhase(.releaseMissing(tag: tag, device: device))
            return false
        case .unreachable:
            setPhase(.releaseOffline)
            return false
        }
    }

    /// The release step on its own, for a launch with no adapter to find it behind (the mock).
    /// Same step, same screens, same holds as inside `dongleGate()`; `.car` because there is no
    /// adapter whose image the release would have to carry.
    private func releaseGate() async {
        while latestTag == nil {
            if await fetchRelease(for: .car) { return }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }

    /// The dongle's interface came back after going away.
    ///
    /// Unplugging mid-drive is handled correctly all the way down — `CarPath` goes unsatisfied,
    /// `CarLink` says the wire is gone, the screen says so — but `dongleGate()` returned for
    /// good when it handed over, so nothing is left watching the dongle. The dongle comes back
    /// having forgotten the car — it keeps the network in RAM only — so it sits idle with no
    /// network, answering nobody, and `CarLink` radars indefinitely with no path back to the
    /// join logic. This is that path, and it is the only hole in an otherwise complete unplug
    /// story.
    ///
    /// Only the dongle half re-runs, and `latestTag` is still held, so the release is not asked
    /// again. Handing back to `.awaitingCar` is enough: `carIdentified` restores
    /// `.ready`/`.updateRequired` on the next hello, which is where the phase was before the
    /// wire went.
    func dongleReturned() async {
        guard CarHost.viaDongle else { return }
        // Nothing to re-ask if the gate never handed over in the first place — a flap during the
        // launch gate is that gate's own business, and `gateRunning` keeps two loops from
        // polling the same address.
        guard !gateRunning else { return }
        guard phase == .awaitingCar || phase == .ready || phase == .updateRequired else { return }
        gateRunning = true
        defer { gateRunning = false }
        dongleHandedOver = false
        await dongleGate()
        dongleHandedOver = true
        setPhase(.awaitingCar)
    }

    /// Poll the dongle until it reports `.readyForCar`, acting on whatever `DongleLink` says is
    /// next at each step, and return. What follows is the caller's: both `startupCheck()` and
    /// `dongleReturned()` — a re-entry after the wire came back — move on to `.awaitingCar`.
    /// The newest release is learned here, once, for both boards (`fetchRelease`): one release
    /// tags both images, and the car is compared against the same tag after its hello.
    private func dongleGate() async {
        // Fetched once, lazily, the first time `/status` actually answers — not up front. The
        // spec's own order is "check whether a dongle is there... if it is, check for a newer
        // version": fetching GitHub before the first presence check would make a phone with
        // nothing plugged in wait on a network round trip just to be told to plug something in.
        while true {
            if phase == .dongleUpdating {
                // The screen owns the adapter right now. Polling it mid-flash would read the
                // silence as "unplugged" and tear down the very view doing the work.
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            let reply = await readStatus()
            // Step 2, once: something is there and is being looked over. Guarded, because this
            // loop re-reads /status forever and must not walk the ladder backwards on every poll.
            if case .status = reply, !sawDongle {
                sawDongle = true
                setPhase(.dongleChecking)
            }
            // Step 3, and a gate rather than a formality: the newest release must be established
            // before anything is decided about the adapter. Retried on every poll until it is —
            // a launch that could not reach GitHub must not proceed on the assumption that
            // nothing has changed, which is exactly what it used to do.
            if case .status = reply, latestTag == nil {
                if !(await fetchRelease(for: .dongle)) {
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
            switch DongleLink.next(reply: reply, latestTag: latestTag,
                                   expectedSSID: CarContract.ssid, rollback: rollbackChoice) {
            case .plugIn:
                // A dongle that is gone is a dongle that will come back knowing nothing: it
                // keeps the car's network in RAM only, so a replug (or its own restart) is a
                // fresh device with an empty configuration. The join budget is per
                // conversation with one dongle, and this is the end of one — so the next
                // dongle to answer gets its one ask, whatever this one spent. Without this,
                // an app that had already given up saw the returning dongle report no
                // network, declined to send it, and sat on «Нет связи» over a dongle that
                // was never told what to look for.
                dongleJoinAttempts = 0
                dongleJoinGaveUp = false
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
                // Handing over, not doing. `FirmwareView` runs the update — the same screen and
                // the same phases the car's update has always used — and this loop stands aside
                // until it says it is finished. It used to do the work itself, blind, behind a
                // spinner and an attempt budget; the budget existed because a headless retry can
                // spin forever unnoticed, and a screen with a failure and a button does not.
                setPhase(.dongleUpdating)
            case .sendCredentials:
                // Once the budget is spent this is the same dead end `.retryJoin` reaches, and
                // it says so: a dongle that keeps reporting a network other than the car's
                // after being told the car's is not "configuring", it is failing to configure.
                setPhase(dongleJoinGaveUp ? .dongleJoinFailed : .dongleSendingNet)
                // No credential state lives here or in DongleClient — CarContract's are opaque
                // constants, read and handed over, never logged or shown (see DongleClient's own
                // doc for why `join`/`retryJoin` are shaped this way).
                await askDongleToJoin(retry: false)
            case .searchingCar:
                setPhase(.carFinding)
            case .waiting:
                setPhase(.dongleConfiguring)
            case .retryJoin:
                // The same rule `.sendCredentials` follows: the failure screen only when there
                // is no ask left to make. With one still owed, the dongle will be searching
                // again by the next poll, and «Ищу машинку» is what is true for that beat.
                setPhase(dongleJoinGaveUp ? .dongleJoinFailed : .carFinding)
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
    /// rejected SSID (400), "the radio refused the join" (500), an undecodable body
    /// and no cable at all were four different faults with four different fixes — flattened
    /// into one `nil`, rendered as one screen telling the user to plug in a dongle that is
    /// plugged in and answering, with nothing written to the log either.
    /// `UpdateClient.upload` already logs its own failures for exactly this reason.
    private func readStatus() async -> DongleReply {
        do {
            let data = try await dongle.statusData()
            let reply = DongleReply.decode(data)
            // Bytes arrived, so this is not silence — but a body that does not decode as a
            // `/status` document is still "answered badly", exactly the fault the doc comment
            // above says must not be folded back into `nil`/silence. Same
            // once-per-distinct-failure dedupe as the catch branch below.
            if case .faulty = reply {
                let what = "body is not a /status document (\(data.count) bytes)"
                if what != lastStatusFailure {
                    lastStatusFailure = what
                    print("dongle \(DongleContract.statusPath): \(what)")
                }
            } else {
                lastStatusFailure = nil
            }
            return reply
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
    /// a 500 means it took them and the radio refused, which are the same screen but very
    /// different bench sessions.
    private func askDongleToJoin(retry: Bool) async {
        // Bounded, and the bound covers BOTH kinds of ask: a dongle that never holds what it
        // is told loops through `.sendCredentials` exactly as tirelessly as a radio that cannot
        // reach the car loops through `.retryJoin`, and neither may run forever.
        guard !dongleJoinGaveUp else { return }
        dongleJoinAttempts += 1
        if dongleJoinAttempts >= Self.maxDongleJoinAttempts { dongleJoinGaveUp = true }
        do {
            let reply: DongleWifiReply
            if retry {
                reply = try await dongle.retryJoin(ssid: CarContract.ssid, password: CarContract.password)
            } else {
                reply = try await dongle.join(ssid: CarContract.ssid, password: CarContract.password)
            }
            print("dongle \(DongleContract.wifiPath): \(reply.state)")
        } catch {
            // `logDescription` names the failure's shape and nothing else — no body, and
            // nothing of what was sent. The credentials never reach a log.
            print("dongle \(DongleContract.wifiPath) (\(retry ? "retry" : "configure")) failed: "
                  + Self.describe(error))
        }
    }

    /// `CarError.logDescription` when it is one — the same vocabulary `UpdateClient.upload`
    /// logs — and a plain description otherwise, which is how a `DecodingError` from a body
    /// this build could not read says which key it choked on.
    private static func describe(_ error: Error) -> String {
        (error as? CarError)?.logDescription ?? String(describing: error)
    }

    /// The user asked whether a newer release exists yet — `FirmwareView`'s rolled-back car
    /// screen keeps the same offer beside its skip. Two halves, both required: re-open the
    /// release fetch (a tag fetched before the rollback screen appeared is exactly the tag that
    /// cannot help), and record what was on offer at the time so `DongleLink` can tell a
    /// genuinely newer image from the one that just rolled back.
    func recheckDongleRollback() {
        rollbackChoice = .recheck(from: latestTag)
        // Clearing the tag is what makes the next poll re-ask GitHub: `fetchRelease` runs while
        // `latestTag == nil`, so this is the recheck actually happening.
        latestTag = nil
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
    /// the very next `.updating` step gets a fresh budget.
    /// `FirmwareView` is done with the adapter — updated, or failed and dismissed. Handing the
    /// phase back to the check is what makes the gate re-decide from a fresh `/status` instead of
    /// trusting whatever the screen concluded.
    func dongleUpdateFinished() { setPhase(.dongleChecking) }

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
}
