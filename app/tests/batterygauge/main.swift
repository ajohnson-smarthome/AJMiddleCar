// Host test for BatteryGauge — what the battery badge shows for a `battery` group (openspec
// `app/drive-hud`, «Приборы показывают связь и намерение пульта», the pack's two scenarios).
//
// The badge is arithmetic over the last frame's `battery`: how much of the icon is filled,
// whether it wears the bolt, which of the palette's three tones it takes, and the numbers of
// its caption. The view (`BatteryBadge`) only draws what this says, so the rule is host-tested
// here and the view is looked at in the gallery.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

func pack(_ state: BatteryState, soc: Int? = 72, mv: Int? = 12310, ma: Int? = 3100, mw: Int? = 38200) -> BatteryInfo {
    BatteryInfo(voltage_mv: mv, current_ma: ma, power_mw: mw, soc_pct: soc, state: state)
}

// The spec's «Пак на три четверти»: 72 % filled, «72 % · 12,3 В · 38 Вт», the text tone.
do {
    let g = BatteryGauge.make(pack(.ok))
    check(g.fill == 0.72, "ok: the icon is filled to soc_pct")
    check(!g.charging, "ok, discharging: no bolt")
    check(g.tone == .text, "ok: the text tone")
    check(g.stats == BatteryGauge.Stats(pct: 72, volts: "12,3", watts: 38), "ok: the caption's numbers")
}

// «Монитора нет»: an empty icon, no numbers («—»), the muted tone.
do {
    let g = BatteryGauge.make(pack(.absent, soc: nil, mv: nil, ma: nil, mw: nil))
    check(g.fill == nil, "absent: the icon is empty")
    check(!g.charging, "absent: no bolt")
    check(g.tone == .muted, "absent: the muted tone")
    check(g.stats == nil, "absent: «—» instead of numbers")
}

// No frame at all reads the same as no monitor — nothing to say, and nothing to warn about.
do {
    let g = BatteryGauge.make(nil)
    check(g.fill == nil && g.stats == nil && g.tone == .muted, "no telemetry: as absent")
}

// «Пак сел»: the fill and the caption in the warning tone.
do {
    let g = BatteryGauge.make(pack(.low, soc: 18, mv: 11100, ma: 2600, mw: 28900))
    check(g.fill == 0.18, "low: filled to soc_pct")
    check(g.tone == .warn, "low: the warning tone")
    check(g.stats == BatteryGauge.Stats(pct: 18, volts: "11,1", watts: 29), "low: the caption's numbers, watts rounded")
}

// The start not determined yet: `ok` with a null soc_pct — the icon is empty, but volts and
// watts are shown.
do {
    let g = BatteryGauge.make(pack(.ok, soc: nil))
    check(g.fill == nil, "soc null: the icon is empty")
    check(g.tone == .text, "soc null at ok: still the text tone")
    check(g.stats == BatteryGauge.Stats(pct: nil, volts: "12,3", watts: 38), "soc null: volts and watts still shown")
}

// Current into the pack is the bolt; zero current is not.
check(BatteryGauge.make(pack(.ok, ma: -1500)).charging, "negative current: the bolt")
check(!BatteryGauge.make(pack(.ok, ma: 0)).charging, "zero current: no bolt")
check(!BatteryGauge.make(pack(.absent, soc: nil, mv: nil, ma: nil, mw: nil)).charging, "absent: no bolt whatever the current")

// The fill is a fraction of the icon, so a percent off the wire's range is held to it.
check(BatteryGauge.make(pack(.ok, soc: 0)).fill == 0, "0 %: empty fill, not a missing one")
check(BatteryGauge.make(pack(.ok, soc: 100)).fill == 1, "100 %: full")
check(BatteryGauge.make(pack(.ok, soc: 140)).fill == 1, "over 100: clamped")
check(BatteryGauge.make(pack(.ok, soc: -3)).fill == 0, "under 0: clamped")

// Volts: one decimal, rounded, the decimal comma of the app's one language; watts: whole.
check(BatteryGauge.make(pack(.ok, mv: 12360)).stats?.volts == "12,4", "12360 mV rounds up to 12,4")
check(BatteryGauge.make(pack(.ok, mv: 12000)).stats?.volts == "12,0", "12000 mV keeps its zero")
check(BatteryGauge.make(pack(.ok, mv: 9950)).stats?.volts == "10,0", "9950 mV carries into the volts")
check(BatteryGauge.make(pack(.ok, mw: 38500)).stats?.watts == 39, "38500 mW rounds to 39")
check(BatteryGauge.make(pack(.ok, mw: 400)).stats?.watts == 0, "400 mW is 0 W")

// A word this build does not know is shown as the car said it: numbers, the text tone.
do {
    let g = BatteryGauge.make(pack(.unknown("cold")))
    check(g.tone == .text && g.fill == 0.72 && g.stats != nil, "unknown state: numbers as at ok")
}

if failures == 0 { print("batterygauge: ok") } else { exit(1) }
