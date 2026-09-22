import Foundation

/// Per-trick settings persisted in UserDefaults: the assembly mode, the per-action durations
/// (ms, one per distinct action — manual mode) and each trick's two geometry parameters.
enum TrickSettings {
    /// UserDefaults.standard in the app; a host test swaps in a suite of its own.
    static var store: UserDefaults = .standard

    /// Every setting of `trick`, as `Tricks.assemble` reads them.
    static func params(for trick: Trick) -> TrickParams {
        TrickParams(mode: mode(for: trick), durs: durations(for: trick),
                    donutDiaCm: donutDiameterCm(), donutCircles: donutCircles(),
                    spinTurns: spinTurns(), spinDurMs: spinDurMs(),
                    fig8DiaCm: fig8Dia(), fig8Eights: fig8Eights(),
                    wiggleAmp: wiggleAmp(), wiggleWags: wiggleWags())
    }

    private static func modeKey(_ id: Int) -> String { "trick.mode.\(id)" }
    /// Geometry unless manual was chosen; an unknown stored word reads as the default.
    static func mode(for trick: Trick) -> TrickMode {
        (store.string(forKey: modeKey(trick.id))).flatMap(TrickMode.init(rawValue:)) ?? .geometry
    }
    static func setMode(_ trick: Trick, _ mode: TrickMode) {
        if mode == .geometry { store.removeObject(forKey: modeKey(trick.id)) }
        else { store.set(mode.rawValue, forKey: modeKey(trick.id)) }
    }

    private static func key(_ id: Int) -> String { "trick.durs.\(id)" }

    static func durations(for trick: Trick) -> [Int] {
        let base = Tricks.baseDurations(trick)
        if let saved = store.array(forKey: key(trick.id)) as? [Int], saved.count == base.count {
            return saved.map { Tricks.clampDur($0) }
        }
        return base
    }
    static func setDuration(_ trick: Trick, action i: Int, ms: Int) {
        var d = durations(for: trick)
        guard d.indices.contains(i) else { return }
        d[i] = Tricks.clampDur(ms)
        store.set(d, forKey: key(trick.id))
    }
    static func reset(_ trick: Trick, action i: Int) {
        var d = durations(for: trick)
        let base = Tricks.baseDurations(trick)
        guard d.indices.contains(i) else { return }
        d[i] = base[i]
        if d == base { store.removeObject(forKey: key(trick.id)) }
        else { store.set(d, forKey: key(trick.id)) }
    }

    private static let donutDiaKey = "trick.donut.diaCm"
    private static func clampDia(_ cm: Int) -> Int {
        Swift.min(Tricks.donutDiaMaxCm, Swift.max(Tricks.donutDiaMinCm, cm))
    }
    static func donutDiameterCm() -> Int {
        clampDia(store.object(forKey: donutDiaKey) as? Int ?? Tricks.donutDiaDefaultCm)
    }
    static func setDonutDiameter(_ cm: Int) {
        store.set(clampDia(cm), forKey: donutDiaKey)
    }
    static func resetDonutDiameter() {
        store.removeObject(forKey: donutDiaKey)
    }

    private static let donutCirclesKey = "trick.donut.circles"
    private static func clampCircles(_ n: Int) -> Int {
        Swift.min(Tricks.donutCirclesMax, Swift.max(Tricks.donutCirclesMin, n))
    }
    static func donutCircles() -> Int {
        clampCircles(store.object(forKey: donutCirclesKey) as? Int ?? Tricks.donutCirclesDefault)
    }
    static func setDonutCircles(_ n: Int) {
        store.set(clampCircles(n), forKey: donutCirclesKey)
    }
    static func resetDonutCircles() {
        store.removeObject(forKey: donutCirclesKey)
    }

    private static let spinTurnsKey = "trick.spin.turns"
    private static func clampSpinTurns(_ n: Int) -> Int {
        Swift.min(Tricks.spinTurnsMax, Swift.max(Tricks.spinTurnsMin, n))
    }
    static func spinTurns() -> Int {
        clampSpinTurns(store.object(forKey: spinTurnsKey) as? Int ?? Tricks.spinTurnsDefault)
    }
    static func setSpinTurns(_ n: Int) {
        store.set(clampSpinTurns(n), forKey: spinTurnsKey)
    }
    static func resetSpinTurns() {
        store.removeObject(forKey: spinTurnsKey)
    }

    private static let spinDurKey = "trick.spin.durMs"
    private static func clampSpinDur(_ ms: Int) -> Int {
        Swift.min(Tricks.spinDurMaxMs, Swift.max(Tricks.spinDurMinMs, ms))
    }
    static func spinDurMs() -> Int {
        clampSpinDur(store.object(forKey: spinDurKey) as? Int ?? Tricks.spinDurDefaultMs)
    }
    static func setSpinDurMs(_ ms: Int) {
        store.set(clampSpinDur(ms), forKey: spinDurKey)
    }
    static func resetSpinDurMs() {
        store.removeObject(forKey: spinDurKey)
    }

    private static let fig8DiaKey = "trick.fig8.dia"
    private static func clampFig8Dia(_ cm: Int) -> Int {
        Swift.min(Tricks.fig8DiaMaxCm, Swift.max(Tricks.fig8DiaMinCm, cm))
    }
    static func fig8Dia() -> Int {
        clampFig8Dia(store.object(forKey: fig8DiaKey) as? Int ?? Tricks.fig8DiaDefaultCm)
    }
    static func setFig8Dia(_ cm: Int) {
        store.set(clampFig8Dia(cm), forKey: fig8DiaKey)
    }
    static func resetFig8Dia() {
        store.removeObject(forKey: fig8DiaKey)
    }

    private static let fig8EightsKey = "trick.fig8.eights"
    private static func clampFig8Eights(_ n: Int) -> Int {
        Swift.min(Tricks.fig8EightsMax, Swift.max(Tricks.fig8EightsMin, n))
    }
    static func fig8Eights() -> Int {
        clampFig8Eights(store.object(forKey: fig8EightsKey) as? Int ?? Tricks.fig8EightsDefault)
    }
    static func setFig8Eights(_ n: Int) {
        store.set(clampFig8Eights(n), forKey: fig8EightsKey)
    }
    static func resetFig8Eights() {
        store.removeObject(forKey: fig8EightsKey)
    }

    private static let wiggleAmpKey = "trick.wiggle.amp"
    private static func clampWiggleAmp(_ a: Double) -> Double {
        Swift.min(Tricks.wiggleAmpMax, Swift.max(Tricks.wiggleAmpMin, a))
    }
    static func wiggleAmp() -> Double {
        clampWiggleAmp(store.object(forKey: wiggleAmpKey) as? Double ?? Tricks.wiggleAmpDefault)
    }
    static func setWiggleAmp(_ a: Double) {
        store.set(clampWiggleAmp(a), forKey: wiggleAmpKey)
    }
    static func resetWiggleAmp() {
        store.removeObject(forKey: wiggleAmpKey)
    }

    private static let wiggleWagsKey = "trick.wiggle.wags"
    private static func clampWiggleWags(_ n: Int) -> Int {
        Swift.min(Tricks.wiggleWagsMax, Swift.max(Tricks.wiggleWagsMin, n))
    }
    static func wiggleWags() -> Int {
        clampWiggleWags(store.object(forKey: wiggleWagsKey) as? Int ?? Tricks.wiggleWagsDefault)
    }
    static func setWiggleWags(_ n: Int) {
        store.set(clampWiggleWags(n), forKey: wiggleWagsKey)
    }
    static func resetWiggleWags() {
        store.removeObject(forKey: wiggleWagsKey)
    }
}
