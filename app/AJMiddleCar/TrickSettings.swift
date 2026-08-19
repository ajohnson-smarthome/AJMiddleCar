import Foundation

/// Per-trick action durations (ms, one per distinct action), persisted in UserDefaults.
enum TrickSettings {
    private static func key(_ id: Int) -> String { "trick.durs.\(id)" }

    static func durations(for trick: Trick) -> [Int] {
        let base = Tricks.baseDurations(trick)
        if let saved = UserDefaults.standard.array(forKey: key(trick.id)) as? [Int], saved.count == base.count {
            return saved.map { Tricks.clampDur($0) }
        }
        return base
    }
    static func setDuration(_ trick: Trick, action i: Int, ms: Int) {
        var d = durations(for: trick)
        guard d.indices.contains(i) else { return }
        d[i] = Tricks.clampDur(ms)
        UserDefaults.standard.set(d, forKey: key(trick.id))
    }
    static func reset(_ trick: Trick, action i: Int) {
        var d = durations(for: trick)
        let base = Tricks.baseDurations(trick)
        guard d.indices.contains(i) else { return }
        d[i] = base[i]
        if d == base { UserDefaults.standard.removeObject(forKey: key(trick.id)) }
        else { UserDefaults.standard.set(d, forKey: key(trick.id)) }
    }

    private static let donutDiaKey = "trick.donut.diaCm"
    private static func clampDia(_ cm: Int) -> Int {
        Swift.min(Tricks.donutDiaMaxCm, Swift.max(Tricks.donutDiaMinCm, cm))
    }
    static func donutDiameterCm() -> Int {
        clampDia(UserDefaults.standard.object(forKey: donutDiaKey) as? Int ?? Tricks.donutDiaDefaultCm)
    }
    static func setDonutDiameter(_ cm: Int) {
        UserDefaults.standard.set(clampDia(cm), forKey: donutDiaKey)
    }
    static func resetDonutDiameter() {
        UserDefaults.standard.removeObject(forKey: donutDiaKey)
    }

    private static let donutCirclesKey = "trick.donut.circles"
    private static func clampCircles(_ n: Int) -> Int {
        Swift.min(Tricks.donutCirclesMax, Swift.max(Tricks.donutCirclesMin, n))
    }
    static func donutCircles() -> Int {
        clampCircles(UserDefaults.standard.object(forKey: donutCirclesKey) as? Int ?? Tricks.donutCirclesDefault)
    }
    static func setDonutCircles(_ n: Int) {
        UserDefaults.standard.set(clampCircles(n), forKey: donutCirclesKey)
    }
    static func resetDonutCircles() {
        UserDefaults.standard.removeObject(forKey: donutCirclesKey)
    }

    private static let spinTurnsKey = "trick.spin.turns"
    private static func clampSpinTurns(_ n: Int) -> Int {
        Swift.min(Tricks.spinTurnsMax, Swift.max(Tricks.spinTurnsMin, n))
    }
    static func spinTurns() -> Int {
        clampSpinTurns(UserDefaults.standard.object(forKey: spinTurnsKey) as? Int ?? Tricks.spinTurnsDefault)
    }
    static func setSpinTurns(_ n: Int) {
        UserDefaults.standard.set(clampSpinTurns(n), forKey: spinTurnsKey)
    }
    static func resetSpinTurns() {
        UserDefaults.standard.removeObject(forKey: spinTurnsKey)
    }

    private static let spinDurKey = "trick.spin.durMs"
    private static func clampSpinDur(_ ms: Int) -> Int {
        Swift.min(Tricks.spinDurMaxMs, Swift.max(Tricks.spinDurMinMs, ms))
    }
    static func spinDurMs() -> Int {
        clampSpinDur(UserDefaults.standard.object(forKey: spinDurKey) as? Int ?? Tricks.spinDurDefaultMs)
    }
    static func setSpinDurMs(_ ms: Int) {
        UserDefaults.standard.set(clampSpinDur(ms), forKey: spinDurKey)
    }
    static func resetSpinDurMs() {
        UserDefaults.standard.removeObject(forKey: spinDurKey)
    }

    private static let fig8DiaKey = "trick.fig8.dia"
    private static func clampFig8Dia(_ cm: Int) -> Int {
        Swift.min(Tricks.fig8DiaMaxCm, Swift.max(Tricks.fig8DiaMinCm, cm))
    }
    static func fig8Dia() -> Int {
        clampFig8Dia(UserDefaults.standard.object(forKey: fig8DiaKey) as? Int ?? Tricks.fig8DiaDefaultCm)
    }
    static func setFig8Dia(_ cm: Int) {
        UserDefaults.standard.set(clampFig8Dia(cm), forKey: fig8DiaKey)
    }
    static func resetFig8Dia() {
        UserDefaults.standard.removeObject(forKey: fig8DiaKey)
    }

    private static let fig8EightsKey = "trick.fig8.eights"
    private static func clampFig8Eights(_ n: Int) -> Int {
        Swift.min(Tricks.fig8EightsMax, Swift.max(Tricks.fig8EightsMin, n))
    }
    static func fig8Eights() -> Int {
        clampFig8Eights(UserDefaults.standard.object(forKey: fig8EightsKey) as? Int ?? Tricks.fig8EightsDefault)
    }
    static func setFig8Eights(_ n: Int) {
        UserDefaults.standard.set(clampFig8Eights(n), forKey: fig8EightsKey)
    }
    static func resetFig8Eights() {
        UserDefaults.standard.removeObject(forKey: fig8EightsKey)
    }

    private static let wiggleAmpKey = "trick.wiggle.amp"
    private static func clampWiggleAmp(_ a: Double) -> Double {
        Swift.min(Tricks.wiggleAmpMax, Swift.max(Tricks.wiggleAmpMin, a))
    }
    static func wiggleAmp() -> Double {
        clampWiggleAmp(UserDefaults.standard.object(forKey: wiggleAmpKey) as? Double ?? Tricks.wiggleAmpDefault)
    }
    static func setWiggleAmp(_ a: Double) {
        UserDefaults.standard.set(clampWiggleAmp(a), forKey: wiggleAmpKey)
    }
    static func resetWiggleAmp() {
        UserDefaults.standard.removeObject(forKey: wiggleAmpKey)
    }

    private static let wiggleWagsKey = "trick.wiggle.wags"
    private static func clampWiggleWags(_ n: Int) -> Int {
        Swift.min(Tricks.wiggleWagsMax, Swift.max(Tricks.wiggleWagsMin, n))
    }
    static func wiggleWags() -> Int {
        clampWiggleWags(UserDefaults.standard.object(forKey: wiggleWagsKey) as? Int ?? Tricks.wiggleWagsDefault)
    }
    static func setWiggleWags(_ n: Int) {
        UserDefaults.standard.set(clampWiggleWags(n), forKey: wiggleWagsKey)
    }
    static func resetWiggleWags() {
        UserDefaults.standard.removeObject(forKey: wiggleWagsKey)
    }
}
