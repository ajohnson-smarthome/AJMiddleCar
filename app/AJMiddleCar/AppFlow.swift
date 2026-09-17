import Foundation

/// The launch ladder, run one rung at a time: `[adapter, car]` through the dongle, `[car]`
/// directly against the mock. Both rungs are answered by the same pure stage (`StageRule`, from
/// one read of the board's `/version`: identity, rollback, version, protocol) driven by `stage()`
/// below — the dongle-prefixed and car-prefixed gates this replaced (`dongleGate()`/`carGate()`)
/// repeated one structure twice; now there is one, and a board is a description (`Board`), not a
/// second copy of the loop.
///
/// It no longer has a `.drive` state. Driving is not a phase the ladder can enter and latch; it is
/// what `CarLink` says is true right now, so a car that goes away mid-session is handled by the
/// screen rather than by a state machine that has already decided.
@MainActor
final class AppFlow: ObservableObject {
    /// One rung of the ladder — the board and its current step. The only screen-bearing phase
    /// there is now, in place of the twenty-odd board-prefixed cases this replaced.
    enum Phase: Equatable {
        case stage(UpdateRules.Device, GateStep)
        case releaseCheck                                            // S4
        case releaseOffline                                          // S5
        case releaseMissing(tag: String, device: UpdateRules.Device) // S6
        /// The ladder is done; `CarLink` owns the screen (radar or drive).
        case awaitingCar
        /// `carIdentified` said the car may drive.
        case ready

        /// The phases whose screen opens the UDP link. Everything mid-ladder is on the false side:
        /// until the ladder hands over there is no session to open, and `.stage(_, .updating)` runs
        /// the forced update over HTTP — a session there would only shout `wrongProto` at the very
        /// car being updated.
        var opensLink: Bool {
            switch self {
            case .awaitingCar, .ready: return true
            case .stage, .releaseCheck, .releaseOffline, .releaseMissing: return false
            }
        }
    }

    @Published var phase: Phase = .stage(.dongle, .seeking)
    /// What is actually on screen. Lags `phase` by at most `PhasePacer.minVisible` per step, so
    /// a launch whose steps resolve instantly is a readable sequence rather than one frame of
    /// strobing. Everything that renders reads this; everything that decides reads `phase`.
    @Published private(set) var shown: Phase = .stage(.dongle, .seeking)
    private var shownAt = Date()
    private var queued: [Phase] = []
    private var pacing = false
    /// The newest release's tag — one for both boards, learned once per launch by `fetchRelease`.
    /// `StageRule.decide` compares each board against it, and so does `carIdentified` on every
    /// hello that changes `fw`. Cleared by `recheckRollback()`, which is what makes the next poll
    /// ask again.
    @Published var latestTag: String?
    let client = UpdateClient()
    /// Shared with `FirmwareView`, which runs the adapter's update through the same screen the
    /// car's goes through — one client, so the ladder and the screen are talking to the same
    /// device over the same connection.
    let dongle = DongleClient()

    /// How often a stage re-reads `/version` (and, for the car through the adapter, `/status`)
    /// while it is not yet current. A judgement, not a measurement: fast enough that "plug it in"
    /// clears within a beat of the cable actually going in, slow enough not to matter next to the
    /// requests it is pacing — `DongleClient` asks for single-flight use, and this is what keeps
    /// every step in this loop to at most one outstanding request at a time.
    private static let donglePollInterval: Duration = .milliseconds(1500)

    /// The ladder for this launch: `[adapter, car]` via the dongle, `[car]` against the mock.
    private var boards: [Board] = []
    /// The rung the runner is on — read by `restart` to decide whether to fall back.
    private var currentRung = 0
    /// A guard asked the ladder to be at this rung or earlier. Consumed at the top of `stage`.
    private var wantRung: Int?
    /// The car's reach state — the join budget — recreated at each entry to the car's stage.
    private var carReach = CarReach()

    /// The ladder's sleep between polls, held so a button can cut it short: «Повторить» on a
    /// wrong-device or rolled-back screen re-asks now rather than at the next 1.5 s tick.
    private var pollSleep: Task<Void, Never>?

    /// Guards against a second runner running while one is already in flight — a second tap on a
    /// retry button whose screen has not yet updated `phase` (a stage's first act is an `await`
    /// on `/version`, up to its timeout, before it writes anything) would otherwise spawn a second
    /// poll loop issuing requests at the dongle's fixed address alongside the first, which is
    /// exactly what `DongleClient` asks callers not to do — and if the first loop has already
    /// handed off to a live drive session, the second would still be out there writing `phase`
    /// out from under it on its own next poll.
    private var gateRunning = false

    /// Set by `recheckRollback()`. See `GateStep.rolledBack` and `RollbackChoice` for why this has
    /// to exist at all: a board's rollback flag does not clear on its own, and the app is the
    /// only OTA path either board has.
    private var rollbackChoice: RollbackChoice = .unanswered

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
        await runLadder(from: 0)
        setPhase(.awaitingCar)
    }

    /// The ladder, rung by rung. `.advance` climbs; `.jump(w)` (a guard, or a lost reach) drops to
    /// rung `w`. `gateRunning` is claimed by every caller before the first `await`, so two runners
    /// never overlap.
    private func runLadder(from start: Int) async {
        boards = buildBoards()
        var i = min(start, boards.count - 1)
        while i < boards.count {
            currentRung = i
            switch await stage(boards[i]) {
            case .advance: i += 1
            case .jump(let w): i = max(0, min(w, boards.count - 1))
            }
        }
    }

    /// `[adapter, car-through-adapter]` when there is a dongle to reach the car through; `[car]`
    /// read directly when there is not (the mock). The rung indices `restart`/`reach` use come from
    /// `rung(of:)`, not from this array, so they are valid before the first build.
    private func buildBoards() -> [Board] {
        if CarHost.viaDongle { return [dongleBoard(), carBoard(reachedThrough: 0)] }
        return [carBoard(reachedThrough: nil)]
    }
    private func rung(of device: UpdateRules.Device) -> Int {
        switch device { case .dongle: return 0; case .car: return CarHost.viaDongle ? 1 : 0 }
    }

    private func dongleBoard() -> Board {
        Board(identity: .init(device: .dongle, expectedDevice: DongleContract.device,
                              proto: DongleContract.proto, silentStep: .absent),
              reachedThrough: nil,
              readVersion: { [dongle] in try await dongle.versionData() },
              reach: { .reached })
    }
    private func carBoard(reachedThrough: Int?) -> Board {
        Board(identity: .init(device: .car, expectedDevice: CarContract.device,
                              proto: CarContract.proto, silentStep: .seeking),
              reachedThrough: reachedThrough,
              readVersion: { try await CarTransport.shared.get(CarContract.versionPath, timeout: 2) },
              reach: reachedThrough == nil ? { .reached }
                                           : { [weak self] in await self?.reachCarViaDongle() ?? .lost })
    }

    /// One poll of "is the adapter in the car's network yet": its `/status` → `CarReach` → show the
    /// step and send the POST it asks for. The car's version is read only once this is `.reached`.
    private func reachCarViaDongle() async -> Reach {
        let reply = await readDongleStatus()
        let (reach, ask) = carReach.next(reply, expectedSSID: CarContract.ssid)
        if let ask { await sendJoin(ask) }
        return reach
    }

    private enum StageExit { case advance; case jump(Int) }

    /// One board's whole stage: reach it, read its `/version`, learn the release if needed, apply
    /// the rule, park on a forced update. Returns when the board is current and ours (`.advance`)
    /// or a guard/lost reach sends the runner elsewhere (`.jump`).
    private func stage(_ board: Board) async -> StageExit {
        if board.identity.device == .car, CarHost.viaDongle { carReach = CarReach() }
        var sawChecking = false
        while true {
            if let w = wantRung { wantRung = nil; return .jump(w) }
            // Parked: FirmwareView owns the board during a forced update. Poll nothing — the car's
            // OTA reboot drops its AP and the adapter would read that as "gone" — just wait for
            // `updateFinished` (which moves the phase off `.updating`) or a guard.
            if case .stage(let d, .updating) = phase, d == board.identity.device {
                await pollPause(); continue
            }
            let reach = await board.reach()
            let version: VersionReply? = reach == .reached ? await readCarOrDongleVersion(board) : nil
            if reach == .reached, let version, !sawChecking, answered(version) {
                sawChecking = true
                setPhase(.stage(board.identity.device, .checking))
            }
            switch StageRule.decide(reach: reach, version: version, board: board.identity,
                                    latestTag: latestTag, rollback: rollbackChoice) {
            case .lost:
                return .jump(board.reachedThrough ?? max(0, currentRung - 1))
            case .needRelease:
                _ = await fetchRelease(for: board.identity.device)   // sets latestTag or a hold phase
                await pollPause(); continue                          // re-decide next poll with the tag
            case .ok:
                return .advance
            case .show(let step):
                if step == .rolledBack || step == .updating { consumeRollbackRecheck() }
                setPhase(.stage(board.identity.device, step))
                await pollPause(); continue
            }
        }
    }

    /// `readVersion(_:_:)` over a board's own `readVersion` closure — the classified read, logged
    /// once per distinct failure (`lastVersionFailure`).
    private func readCarOrDongleVersion(_ board: Board) async -> VersionReply {
        await readVersion(board.identity.device.rawValue, board.readVersion)
    }
    private func answered(_ reply: VersionReply) -> Bool {
        switch reply { case .version, .absent: return true; case .silent, .faulty, .denied: return false }
    }

    /// Step 3, one attempt: ask GitHub for the newest release and adopt its tag. The release is
    /// one for both boards — one tag, two images — so this runs once per launch, and `StageRule`
    /// compares both boards against the tag it leaves in `latestTag`, and so does `carIdentified`
    /// on every hello that changes `fw`, and on every hand-over. `device` only says which image's
    /// presence to insist on.
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

    /// The ladder's pause between polls — `donglePollInterval`, unless `wakePoll()` cuts it short.
    ///
    /// The sleep lives in its own task so a button can cancel it without cancelling the stage;
    /// the handler passes the stage's own cancellation through, so a cancelled loop does not
    /// sit out the rest of the interval first.
    private func pollPause() async {
        let t = Task<Void, Never> { try? await Task.sleep(for: Self.donglePollInterval) }
        pollSleep = t
        await withTaskCancellationHandler { await t.value } onCancel: { t.cancel() }
    }

    /// Cut the current poll pause short — the next read happens immediately.
    func wakePoll() { pollSleep?.cancel() }

    /// One POST asking the adapter to join the car's network. `configure` the first time,
    /// `retry` after — `DongleClient` keeps them apart on purpose. Failures are logged, never
    /// swallowed; the credentials never reach a log.
    private func sendJoin(_ ask: CarReach.Ask) async {
        do {
            let reply: DongleWifiReply
            switch ask {
            case .configure: reply = try await dongle.join(ssid: CarContract.ssid, password: CarContract.password)
            case .retry:     reply = try await dongle.retryJoin(ssid: CarContract.ssid, password: CarContract.password)
            }
            print("dongle \(DongleContract.wifiPath): \(reply.state)")
        } catch {
            print("dongle \(DongleContract.wifiPath) (\(ask == .configure ? "configure" : "retry")) failed: " + Self.describe(error))
        }
    }

    /// A post-gate guard fired: the ladder should be at `device`'s rung or earlier. Not running →
    /// run from there. Running past it → the current stage falls back until it is there. Running at
    /// it or earlier → nothing (it will reach it). `restart(from: .car)` re-enters the car's stage
    /// in place; `restart(from: .dongle)` walks back to the adapter.
    func restart(from device: UpdateRules.Device) {
        guard CarHost.viaDongle || device == .car else { return }
        let target = rung(of: device)
        guard gateRunning else {
            gateRunning = true
            Task { @MainActor in
                defer { self.gateRunning = false }
                await self.runLadder(from: target)
                self.setPhase(.awaitingCar)
            }
            return
        }
        wantRung = min(wantRung ?? currentRung, target)
        wakePoll()
    }

    /// «Повторить» on the join-failed screen: a fresh budget, spent from the next reach.
    func retryJoin() { carReach.retry(); wakePoll() }

    /// `FirmwareView` finished a forced update of `device`. Move the phase off `.updating` so the
    /// parked stage re-reaches and re-decides from a fresh `/version`; if the ladder is not running
    /// (a forced update raised by `carIdentified` mid-session), run it from that board's rung.
    func updateFinished(_ device: UpdateRules.Device) {
        guard case .stage(let d, .updating) = phase, d == device else { return }
        setPhase(.stage(device, .checking))
        if gateRunning {
            wakePoll()          // unpark the running stage
        } else {
            gateRunning = true
            Task { @MainActor in
                defer { self.gateRunning = false }
                await self.runLadder(from: self.rung(of: device))
                self.setPhase(.awaitingCar)
            }
        }
    }

    /// The button each `.stage` step can carry, if any — wired into `ConnectView.onRetry`.
    func retryAction(for step: GateStep) -> (() -> Void)? {
        switch step {
        case .wrongDevice: return { [weak self] in self?.wakePoll() }
        case .rolledBack:  return { [weak self] in self?.recheckRollback() }
        case .joinFailed:  return { [weak self] in self?.retryJoin() }
        default:           return nil
        }
    }

    /// Write `phase` only when it actually moves.
    ///
    /// A stage re-decides the phase on every poll and assigns it unconditionally, and
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

    /// `CarError.logDescription` when it is one — the same vocabulary `UpdateClient.upload`
    /// logs — and a plain description otherwise, which is how a `DecodingError` from a body
    /// this build could not read says which key it choked on.
    private static func describe(_ error: Error) -> String {
        (error as? CarError)?.logDescription ?? String(describing: error)
    }

    /// The user asked whether a newer release exists yet — the one button on either board's
    /// rolled-back screen (`ConnectView.Situation.stage(_, .rolledBack)`). Two halves, both
    /// required: re-open the release fetch (a tag fetched before the rollback screen appeared is
    /// exactly the tag that cannot help), and record what was on offer at the time so
    /// `StageRule` can tell a genuinely newer image from the one that just rolled back.
    func recheckRollback() {
        rollbackChoice = .recheck(from: latestTag)
        // Clearing the tag is what makes the next poll re-ask GitHub: `fetchRelease` runs while
        // `latestTag == nil`, so this is the recheck actually happening — and it happens now,
        // not at the next tick.
        latestTag = nil
        wakePoll()
    }

    /// A recheck is one look, not a standing permission — spent as soon as `StageRule` has
    /// answered with it, whichever way it answered.
    private func consumeRollbackRecheck() {
        if case .recheck = rollbackChoice { rollbackChoice = .unanswered }
    }

    /// The car said who it is, in its hello reply. Re-evaluated every time, not once: a car that
    /// reboots into a different build after an OTA is the same question asked again.
    ///
    /// A nil `fw` is "we have not met the car yet", which is not the same as "the car is behind"
    /// — forcing an update against a car that never answered would be a screen with nothing to
    /// flash.
    func carIdentified(fw: String?) {
        guard let fw else { return }
        guard phase == .awaitingCar || phase == .ready else { return }
        if GateRule.mayDrive(deviceBuild: UpdateClient.buildNumber(fw),
                             latestBuild: UpdateClient.buildNumber(latestTag)) {
            setPhase(.ready)
        } else {
            restart(from: .car)   // re-enter the car's stage; the rule raises .updating from /version
        }
    }
}
