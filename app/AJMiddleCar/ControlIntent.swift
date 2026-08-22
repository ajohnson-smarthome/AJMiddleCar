import Foundation

/// The one command authority: everything that wants to move the car says so here, and this is the
/// only thing that writes the transport's outgoing command.
///
/// Before, the drive screen held the trick's task, the joystick wrote the outbox directly, and
/// "the joystick interrupts a trick" was a call ordering the two by hand at every touch site.
@MainActor
final class ControlIntent: ObservableObject {
    @Published private(set) var t: Double = 0
    @Published private(set) var y: Double = 0
    @Published private(set) var runningTrick: Trick?
    @Published private(set) var trickStartedAt: Date?

    private var core = IntentCore()
    private var trickTask: Task<Void, Never>?
    private let transport: CarTransport
    private let config: ConfigStore

    /// `config` defaults to the shared store; it is a parameter only so a test can hand in
    /// another one. (Not a default argument: those are evaluated at the call site, which is not
    /// necessarily the main actor.)
    init(transport: CarTransport = .shared, config: ConfigStore? = nil) {
        self.transport = transport
        self.config = config ?? .shared
    }

    /// A joystick, a gamepad stick, or anything else the driver did with their hands. Synchronous
    /// end to end: the command is the driver's before this returns, and any trick is disowned in
    /// the same statement.
    func manual(t: Double, y: Double) {
        if core.manual(t: t, y: y) { clearTrick() }
        publish()
    }

    /// Start a trick. **No I/O on this path**: the geometry comes from the config cache, so the
    /// first step goes out on the next 10 Hz tick instead of after two REST round trips that
    /// could time out with the FAB already showing a progress ring.
    func startTrick(_ base: Trick) {
        let trick = Self.build(base,
                               vmaxMS: Self.vmax(config.wheel.value),
                               trackM: Self.track(config.dims.value))
        let epoch = core.beginTrick()
        runningTrick = trick
        trickStartedAt = Date()
        trickTask?.cancel()
        trickTask = Task { [weak self] in
            for step in trick.steps {
                guard let self, self.core.trickStep(epoch: epoch, t: step.t, y: step.y) else { return }
                self.publish()
                try? await Task.sleep(for: .milliseconds(step.ms))
                if Task.isCancelled { return }
            }
            guard let self else { return }
            if self.core.endTrick(epoch: epoch, stop: true) {   // ran to its end → stop
                self.clearTrick()
                self.publish()
            }
        }
    }

    /// The ⏹ button: stop the car as well as the trick.
    func stopTrick() {
        core.endTrick(epoch: core.epoch, stop: true)
        clearTrick()
        publish()
    }

    /// Leaving the drive screen: no command of ours should outlive it.
    func neutral() {
        core.endTrick(epoch: core.epoch, stop: true)
        core.manual(t: 0, y: 0)
        clearTrick()
        publish()
    }

    private func clearTrick() {
        trickTask?.cancel()
        trickTask = nil
        runningTrick = nil
        trickStartedAt = nil
    }

    private func publish() {
        t = core.t
        y = core.y
        transport.setCommand(t: core.t, y: core.y)
    }

    // MARK: - trick geometry, from the cache

    /// Linear speed (m/s) from the car's wheel and motor params, with the nominal fallback.
    static func vmax(_ w: Wheel?) -> Double {
        guard let w, let rpm = MotorPresets.match(ppr: w.ppr, gearX100: w.gear_x100, quad: w.quad)?.rpm
        else { return Tricks.donutNominalVmaxMS }
        return Double.pi * (Double(w.diameter_mm) / 1000) * Double(rpm) / 60
    }

    /// Track (m) from the car's dimensions, with the nominal fallback.
    static func track(_ d: Dims?) -> Double {
        d.map { Double($0.track_mm) / 1000 } ?? Tricks.donutTrackFallbackM
    }

    /// Rebuild a trick from its stored settings. Every branch is pure arithmetic — the reason
    /// this can run on the tap rather than after a fetch.
    static func build(_ base: Trick, vmaxMS: Double, trackM: Double) -> Trick {
        switch base.id {
        case Tricks.spin.id:
            return Tricks.spinTrick(turns: TrickSettings.spinTurns(),
                                    durationMs: TrickSettings.spinDurMs(),
                                    vmaxMS: vmaxMS, trackM: trackM)
        case Tricks.donut.id:
            return Tricks.donutTrick(diameterCm: Double(TrickSettings.donutDiameterCm()),
                                     circles: TrickSettings.donutCircles(),
                                     vmaxMS: vmaxMS, trackM: trackM)
        case Tricks.figure8.id:
            return Tricks.figure8Trick(diameterCm: Double(TrickSettings.fig8Dia()),
                                       eights: TrickSettings.fig8Eights(),
                                       vmaxMS: vmaxMS, trackM: trackM)
        case Tricks.wiggle.id:
            return Tricks.wiggleTrick(amplitude: TrickSettings.wiggleAmp(),
                                      wags: TrickSettings.wiggleWags())
        default:
            return Tricks.withDurations(base, TrickSettings.durations(for: base))
        }
    }
}
