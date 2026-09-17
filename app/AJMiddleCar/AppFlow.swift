import Foundation

/// The launch gate, in the order the spec states it: is a dongle there → learn the newest
/// release (once, for both boards) → update the dongle if it is behind → tell it which network
/// to join if it has not been told → ask the car its `/version` through the relay → force the
/// update if the car is behind → hand over. Against the mock there is no dongle, so the release
/// step runs on its own and the car is asked directly.
///
/// The dongle half and the car half are answered by the same pure rule — `VersionRule`, from
/// one read of either board's `/version`: identity, rollback, version, protocol — with
/// `DongleLink` for "what does the dongle need next" on the network side, and
/// `GateRule`/`UpdateRules` for the hello's re-check of the car. They share one `Phase`, one
/// `latestTag` and one `startupCheck()` entry point, because from the user's seat this has
/// always been a single gate, not two gates that happen to run back to back.
///
/// It no longer has a `.drive` state. Driving is not a phase the gate can enter and latch; it is
/// what `CarLink` says is true right now, so a car that goes away mid-session is handled by the
/// screen rather than by a state machine that has already decided.
@MainActor
final class AppFlow: ObservableObject {
    enum Phase: Equatable {
        /// Before the first `/version` has come back at all — not yet known whether a dongle is
        /// even attached. Distinct from `.dongleAbsent` (which is a definite "nothing answered"
        /// after actually asking): the two would otherwise flash "plug it in" at every cold
        /// launch, dongle attached or not, for as long as the first request takes.
        case checkDongle
        /// Nothing answered `/version` — `VersionRule.step(reply: .silent, ...)`'s own step, not
        /// an error the flow invents separately.
        case dongleAbsent
        /// Step 2. Something answered at the adapter's address and is being looked over. Brief
        /// by nature — the reply is already in hand when this is set — which is exactly why
        /// `PhasePacer` exists: without it this step would never be legible.
        case dongleChecking
        /// Step 3. Asking GitHub for the newest release — once per launch, for both boards: one
        /// release tags both images, so the tag learned here is what the adapter is compared
        /// against now and the car in its own check (`carGate()`). Had no phase at all before,
        /// so this wait happened behind whatever screen preceded it.
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
        /// (`VersionStep.wrongDevice`). Carries what it called itself, so the screen can name it.
        case dongleWrong(device: String)
        /// The dongle's own firmware is behind the latest release; downloading and flashing it,
        /// before anything else in this sequence touches the car. Reused for the short reboot
        /// The screen is `FirmwareView`, the same one the car's update uses.
        case dongleUpdating
        /// The dongle's bootloader reverted its last update. Standing until the user answers —
        /// the rollback flag itself does not clear until a LATER OTA to that slot succeeds, so
        /// without an answer this phase would never release. One answer:
        /// `recheckRollback()` asks whether a newer release exists yet, and it is the only
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
        /// The car's own check, the adapter's S3 mirrored: `GET /version` through the relay,
        /// polled until the car answers — it is on the adapter's network, so this is a reboot
        /// or a slow AP, not an absent car — and decided by the same `VersionRule`.
        case carChecking
        /// The car's `/version` named another device. Decided here, before any hello.
        case carWrong(device: String)
        /// The car's bootloader reverted its last update — the car's S10.
        case carRolledBack
        /// A board that is not behind the release but speaks a protocol this app does not: the
        /// board is newer than the app. Nothing to do here but say so; the poll goes on so the
        /// screen leaves by itself if the board is reflashed.
        case appBehind(device: UpdateRules.Device, proto: Int)
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
        /// `.updateRequired` is on the false side now: the forced update runs over HTTP through
        /// `/version` and `/ota`, and a session opened behind it would only shout `wrongProto`
        /// at a car whose protocol is the reason it is being updated.
        var opensLink: Bool {
            switch self {
            case .awaitingCar, .ready: return true
            case .checkDongle, .dongleAbsent, .dongleChecking, .releaseCheck, .carFinding,
                 .releaseOffline, .releaseMissing,
                 .dongleFault, .dongleDenied, .dongleWrong,
                 .dongleUpdating, .dongleRolledBack,
                 .dongleSendingNet, .dongleConfiguring, .dongleJoinFailed,
                 .carChecking, .carWrong, .carRolledBack, .appBehind,
                 .updateRequired: return false
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
    /// adapter). `VersionRule.step` compares each board against it — the adapter in
    /// `dongleGate()`, the car in `carGate()` — and `carIdentified` the car again on every
    /// hello. Cleared by `recheckRollback()`, which is what makes the next poll ask again.
    @Published var latestTag: String?
    let client = UpdateClient()
    /// Shared with `FirmwareView`, which now runs the adapter's update through the same screen
    /// the car's goes through — one client, so the gate and the screen are talking to the same
    /// device over the same connection.
    let dongle = DongleClient()

    /// How often `dongleGate()` re-reads `/version` (and, past `.ok`, `/status`) while it is
    /// not yet `.readyForCar`, and how often `carGate()` re-reads the car's `/version`. A
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

    /// Whether `dongleGate()` has handed over this session. Set by `runGates()` right after
    /// that hand-over, and cleared by it again when `carGate()` hands back — the adapter dropped
    /// or lost the car's network — so the adapter's gate runs once more. `dongleReturned()`
    /// clears it before its own re-run for the same reason.
    private var dongleHandedOver = false

    /// Guards against a second `startupCheck()` running while one is already in flight — a
    /// second tap on a retry button whose screen has not yet updated `phase` (`dongleGate()`'s
    /// first act is an `await` on `/version`, up to its timeout, before it writes anything) would
    /// otherwise spawn a second poll loop issuing requests at the dongle's fixed address
    /// alongside the first, which is exactly what `DongleClient` asks callers not to do — and if
    /// the first loop has already handed off to a live drive session, the second would still be
    /// out there writing `phase` out from under it on its own next poll.
    private var gateRunning = false

    /// Set by `recheckRollback()`. See `Phase.dongleRolledBack`, `Phase.carRolledBack` and
    /// `RollbackChoice` for why this has to exist at all: a board's rollback flag does not
    /// clear on its own, and the app is the only OTA path either board has.
    private var rollbackChoice: RollbackChoice = .unanswered

    /// Whether the adapter's `/version` has ever answered this launch — the trigger for step 2,
    /// and nothing else.
    private var sawDongle = false
    private var dongleJoinAttempts = 0
    private var dongleJoinGaveUp = false
    /// The last `/status` failure written to the log — see `readDongleStatus()` for why it is
    /// remembered at all.
    private var lastStatusFailure: String?
    /// The same, for `/version` of either board — see `readVersion(_:_:)`.
    private var lastVersionFailure: String?

    /// Entry point. Re-entrant calls while a run is already in flight are ignored — see
    /// `gateRunning`'s own doc.
    func startupCheck() async {
        guard !gateRunning else { return }
        gateRunning = true
        defer { gateRunning = false }
        UpdateClient.migrateCacheIfNeeded()
        await runGates()
        setPhase(.awaitingCar)
    }

    /// The adapter's gate, then the car's, until both agree: the car's gate hands back when the
    /// adapter it talks through has dropped or lost the car's network, and the adapter's gate
    /// is then run again (it forgets the network on every replug).
    ///
    /// The dongle half runs wherever there is a dongle to ask: every device, and a simulator
    /// launched with `-viaDongle` (CarHost) — the adapter on the Mac's USB, the simulator as
    /// the phone. It learns the newest release on the way (step 3). Against the mock there
    /// is no dongle and nothing stands in for one, so the release step runs on its own; the
    /// car itself is then asked directly, the same `carGate()` either way.
    private func runGates() async {
        while true {
            if CarHost.viaDongle {
                if !dongleHandedOver {
                    await dongleGate()
                    dongleHandedOver = true
                }
            } else {
                await releaseGate()
            }
            if await carGate() { return }
            dongleHandedOver = false
        }
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
    /// Only the dongle half re-runs, and then the car's check: the adapter came back knowing
    /// nothing, and the car may have been reflashed or restarted meanwhile. `latestTag` is still
    /// held, so the release is not asked again.
    func dongleReturned() async {
        guard CarHost.viaDongle else { return }
        // Nothing to re-ask if the gate never handed over in the first place — a flap during the
        // launch gate is that gate's own business, and `gateRunning` keeps two loops from
        // polling the same address.
        guard !gateRunning else { return }
        switch phase {
        case .awaitingCar, .ready, .updateRequired,
             // The car-side phases too, in case the link was open behind one of them.
             .carChecking, .carWrong, .carRolledBack, .appBehind: break
        default: return
        }
        gateRunning = true
        defer { gateRunning = false }
        dongleHandedOver = false
        await runGates()
        setPhase(.awaitingCar)
    }

    /// Poll the dongle until it reports `.readyForCar` and return. Each poll is `/version`
    /// first — identity, rollback, version and protocol, decided by `VersionRule` — and only
    /// after `.ok` the `/status` document, whose shape depends on the protocol just vouched
    /// for, decided by `DongleLink`. What follows is the caller's (`runGates()`): the car's own
    /// gate. The newest release is learned here, once, for both boards (`fetchRelease`): one
    /// release tags both images, and the car is compared against the same tag in `carGate()`.
    private func dongleGate() async {
        // Fetched once, lazily, the first time `/version` actually answers — not up front. The
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
            let version = await readVersion("dongle") { try await self.dongle.versionData() }
            // Step 2, once: something is there and is being looked over. Guarded, because this
            // loop re-reads /version forever and must not walk the ladder backwards on every poll.
            if case .version = version, !sawDongle {
                sawDongle = true
                setPhase(.dongleChecking)
            }
            // Step 3: the newest release must be established before anything is decided about
            // the adapter — but only once something has answered at all. Retried on every poll
            // until it is: a launch that could not reach GitHub must not proceed on the
            // assumption that nothing has changed.
            if case .version = version, latestTag == nil {
                if !(await fetchRelease(for: .dongle)) {
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
            if case .absent = version, latestTag == nil {
                // A board older than /version is still a board: the release is needed to update it.
                if !(await fetchRelease(for: .dongle)) {
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
            // Identity, rollback, version, protocol — the same rule the car's gate uses.
            var proceed = false
            switch VersionRule.step(reply: version, expectedDevice: DongleContract.device,
                                    // Unreachable with nil: see the two fetches above.
                                    latestTag: latestTag ?? "", appProto: DongleContract.proto,
                                    rollback: rollbackChoice) {
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
            case .wrongDevice(let name):
                setPhase(.dongleWrong(device: name))
            case .rolledBack:
                // One look per ask: a recheck that found nothing newer is spent here, so the
                // screen comes back with its button instead of re-asking GitHub on every poll
                // from a permission the user gave once.
                consumeRollbackRecheck()
                setPhase(.dongleRolledBack)
            case .updating:
                consumeRollbackRecheck()
                // Handing over, not doing. `FirmwareView` runs the update — the same screen and
                // the same phases the car's update has always used — and this loop stands aside
                // until `dongleUpdateFinished()` puts the phase back to `.dongleChecking`. It
                // used to do the work itself, blind, behind a spinner and an attempt budget; the
                // budget existed because a headless retry can spin forever unnoticed, and a
                // screen with a failure and a button does not.
                setPhase(.dongleUpdating)
            case .appBehind(let proto):
                setPhase(.appBehind(device: .dongle, proto: proto))
            case .ok:
                proceed = true
            }
            if !proceed {
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            // The version agrees, so the protocol-dependent document may be read: the network half.
            let status: DongleStatus
            switch await readDongleStatus() {
            case .status(let s): status = s
            case .silent, .faulty, .denied:
                // Answered /version a moment ago and not /status: a reboot in between. Ask again.
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            switch DongleLink.next(status: status, expectedSSID: CarContract.ssid) {
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

    /// The car's own check — the adapter's steps 2–3 mirrored. `GET /version` through the
    /// relay until the car answers, then the same `VersionRule`. `.silent` here is a car that
    /// is on the adapter's network (the adapter said `connected`) but not answering HTTP yet — a
    /// reboot, a slow AP — so it is a hold, not "plug it in". Returns once the car is ours,
    /// current and speaking our protocol; every other outcome is a phase this loop keeps
    /// re-deciding from the next read.
    private func carGate() async -> Bool {
        while true {
            if phase == .updateRequired {
                // `FirmwareView` owns the car right now; polling it mid-flash would read the
                // silence as a reboot that never ends.
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            let version = await readVersion("car") {
                try await CarTransport.shared.get(CarContract.versionPath, timeout: 2)
            }
            if latestTag == nil, !(await fetchRelease(for: .car)) {
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            switch VersionRule.step(reply: version, expectedDevice: CarContract.device,
                                    latestTag: latestTag ?? "", appProto: CarContract.proto,
                                    rollback: rollbackChoice) {
            case .plugIn, .faulty:
                // Silence through the relay is a rebooting car — unless the relay itself is
                // gone: the adapter unplugged (it forgets the car's network) or dropped off it.
                // The link is not open in this phase, so nobody else would notice; ask the
                // adapter and hand back to its gate when it is not joined any more.
                if CarHost.viaDongle, !(await adapterStillJoined()) { return false }
                setPhase(.carChecking)
            case .accessDenied:
                setPhase(.dongleDenied)
            case .wrongDevice(let name):
                setPhase(.carWrong(device: name))
            case .rolledBack:
                consumeRollbackRecheck()
                setPhase(.carRolledBack)
            case .updating:
                consumeRollbackRecheck()
                setPhase(.updateRequired)
            case .appBehind(let proto):
                setPhase(.appBehind(device: .car, proto: proto))
            case .ok:
                return true
            }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }

    /// Whether the adapter still reports `connected` to the car's network — the one question
    /// `carGate()` asks when the car goes silent through the relay.
    private func adapterStillJoined() async -> Bool {
        guard case .status(let s) = await readDongleStatus() else { return false }
        return DongleLink.next(status: s, expectedSSID: CarContract.ssid) == .readyForCar
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
    private func readDongleStatus() async -> DongleStatusReply {
        do {
            let data = try await dongle.statusData()
            let reply = DongleStatusReply.decode(data)
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
            return DongleStatusReply.of(error)
        }
    }

    /// One `/version` read of either board, classified rather than collapsed into an optional,
    /// and logged once per distinct failure (`lastVersionFailure`) — the same discipline as
    /// `readDongleStatus()`, for the same reason.
    private func readVersion(_ name: String, _ get: () async throws -> Data) async -> VersionReply {
        do {
            let data = try await get()
            let reply = VersionReply.decode(data)
            if case .faulty = reply {
                let what = "\(name): body is not a /version document (\(data.count) bytes)"
                if what != lastVersionFailure { lastVersionFailure = what; print(what) }
            } else {
                lastVersionFailure = nil
            }
            return reply
        } catch {
            let what = "\(name) /version failed: \(Self.describe(error))"
            if what != lastVersionFailure { lastVersionFailure = what; print(what) }
            return VersionReply.of(error)
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

    /// The user asked whether a newer release exists yet — the one button on either board's
    /// rolled-back screen (`ConnectView.Situation.rolledBack(device:)`). Two halves, both
    /// required: re-open the release fetch (a tag fetched before the rollback screen appeared is
    /// exactly the tag that cannot help), and record what was on offer at the time so
    /// `VersionRule` can tell a genuinely newer image from the one that just rolled back.
    func recheckRollback() {
        rollbackChoice = .recheck(from: latestTag)
        // Clearing the tag is what makes the next poll re-ask GitHub: `fetchRelease` runs while
        // `latestTag == nil`, so this is the recheck actually happening.
        latestTag = nil
    }

    /// A recheck is one look, not a standing permission — spent as soon as `VersionRule` has
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

    /// Forced FirmwareView signals completion: re-decide from a fresh /version. If the launch's
    /// own gate loop is still there (it parks while `.updateRequired`), `.carChecking` wakes it;
    /// otherwise — a forced update raised by `carIdentified` mid-session — run the gates again.
    func updateFinished() {
        guard phase == .updateRequired else { return }
        setPhase(.carChecking)
        guard !gateRunning else { return }
        Task { @MainActor in
            self.gateRunning = true
            defer { self.gateRunning = false }
            await self.runGates()
            self.setPhase(.awaitingCar)
        }
    }
}

/// What one read of the adapter's `/status` produced. Read only after `VersionRule` said `.ok`
/// — the document's shape depends on the protocol `/version` just vouched for — so this is the
/// flow's own classification of the network half, not a second identity check. The transport
/// vocabulary is `VersionReply.of`'s; a 404 here is a fault, not an older board: a board old
/// enough to lack `/version` never reaches this read.
private enum DongleStatusReply {
    /// A `/status` document this build could decode.
    case status(DongleStatus)
    /// Nothing answered: no cable, a refused connection, a deadline that expired with no bytes.
    case silent
    /// Something answered and it was not usable: an HTTP error status, a truncated stream, or a
    /// body that did not decode. Whatever else is true, a device is there and talking.
    case faulty
    /// iOS refused to let the request leave the phone at all: local-network access is denied.
    case denied

    /// Read a `/status` body as a document, else as a fault.
    static func decode(_ data: Data) -> DongleStatusReply {
        if let s = try? JSONDecoder().decode(DongleStatus.self, from: data) { return .status(s) }
        return .faulty
    }

    /// Classify what `DongleClient.statusData()` threw — `VersionReply.of`'s rule, with its
    /// one `/version`-specific verdict (404 → `.absent`) folded into `.faulty`.
    static func of(_ error: Error) -> DongleStatusReply {
        switch VersionReply.of(error) {
        case .version: return .faulty      // `of` never returns a document; kept for exhaustiveness
        case .absent, .faulty: return .faulty
        case .silent: return .silent
        case .denied: return .denied
        }
    }
}
