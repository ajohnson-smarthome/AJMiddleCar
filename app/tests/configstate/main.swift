// Host test for the configuration state machine. Run with swiftc.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// Unknown renders as nothing at all. This is the fix: a view that asks for a value while the
// read has not landed must get nil, not the app's fallback dressed as the car's configuration.
let unknown = ConfigState<Wheel>.unknown
check(unknown.value == nil, "unknown has no value")
check(ConfigState<Wheel>.failed(.timeout(3)).value == nil, "a failed read has no value either")

// ...and a value nobody read cannot be written back.
check(unknown.afterSaveRequest(Wheel.default) == nil, "unknown refuses a save")
check(ConfigState<Wheel>.failed(.refused).afterSaveRequest(Wheel.default) == nil,
      "a failed read refuses a save")

// A read that lands is the car's value.
let carsOwn = Wheel(diameter_mm: 70, encoder_ppr: 12, gear_ratio: 9.6, quadrature: 2)
let loaded = ConfigState<Wheel>.afterLoad(.success(carsOwn))
check(loaded == .loaded(carsOwn) && loaded.value == carsOwn, "a read becomes the value")
check(ConfigState<Wheel>.afterLoad(.failure(.timeout(3))) == .failed(.timeout(3)), "a failed read")

// A write of the same value is not a write: no request, no NVS wear.
check(loaded.afterSaveRequest(carsOwn) == nil, "saving the same value is refused")

// A write of a different value goes through saving, and the value stays readable while it does.
let edited = Wheel(diameter_mm: 65, encoder_ppr: 12, gear_ratio: 9.6, quadrature: 2)
guard let saving = loaded.afterSaveRequest(edited) else {
    print("FAIL: a changed value must be saveable"); exit(1)
}
check(saving == .saving(edited), "a changed value enters saving")
check(saving.value == edited, "the pending value is what the screen shows")
check(ConfigState<Wheel>.afterSave(.success(edited)) == .loaded(edited), "the car took it")

// A write that fails says so, and stops claiming to know the car's value: what is on the car
// after a failed POST is precisely what we cannot say.
let failed = ConfigState<Wheel>.afterSave(.failure(.http(status: 400, body: Data())))
check(failed == .failed(.http(status: 400, body: Data())), "a failed write is recorded")
check(failed.value == nil, "and stops pretending to know the car's value")
check(failed.error == .http(status: 400, body: Data()), "the reason is kept")

// Every generated domain is one of these.
check(Ramp.key == "ramp" && Trim.key == "trim" && Recovery.key == "recovery"
      && Wheel.key == "wheel" && Chassis.key == "chassis", "the five domains")
check(Wheel.pick(from: Wheel.wrap(carsOwn)) == carsOwn, "wrap then pick is the identity")

// `storage.reset_at_boot` (openspec `app/settings`, «Кэш доменов…» → «Хранилище машинки
// стёрто»): the car booted with a wiped store, so whatever the caches hold is not what it
// holds. One event per car boot — the caches are dropped and the user told once; a later
// `/status` from the same boot (a session reopened, the firmware screen re-asking) changes
// nothing, since what was read after the drop is that boot's truth (AJM-99).
do {
    var r = StorageReset()
    check(r.status(resetAtBoot: false, uptimeS: 12, now: 1000) == false, "no reset: nothing to do")
    check(r.status(resetAtBoot: true, uptimeS: 12, now: 1000) == true, "reset at this boot: drop the caches and say so")
    check(r.status(resetAtBoot: true, uptimeS: 42, now: 1030) == false, "the same boot thirty seconds on: already acted on")
    check(r.status(resetAtBoot: true, uptimeS: 900, now: 1888) == false, "still the same boot, much later")
    // A car rebooted with the flag again (another migration, or a bench erase): a new event.
    check(r.status(resetAtBoot: true, uptimeS: 5, now: 2000) == true, "a different boot with the flag: a new reset")
    check(r.status(resetAtBoot: true, uptimeS: 6, now: 2001) == false, "and that one is remembered too")
}

// The boot is told apart by its instant on the app's clock (now − uptime), not by the uptime
// growing: a session reopened after a reboot can well read a larger uptime than the last
// reading of the previous boot did.
do {
    var r = StorageReset()
    check(r.status(resetAtBoot: true, uptimeS: 10, now: 1000) == true, "first boot, read at 10 s")
    check(r.status(resetAtBoot: true, uptimeS: 20, now: 1100) == true, "a reboot that has been up longer than the last reading: new boot")
}

// Two readings of one boot never agree to the second — integer uptime, a request in flight,
// the retry that landed — so a few seconds of slack is the same boot.
do {
    var r = StorageReset()
    check(r.status(resetAtBoot: true, uptimeS: 10, now: 1000) == true, "first reading")
    check(r.status(resetAtBoot: true, uptimeS: 13, now: 1004.9) == false, "a reading two seconds off the boot instant: the same boot")
}

// A flag that is false says nothing about boots: the memory is only ever of a reset.
do {
    var r = StorageReset()
    check(r.status(resetAtBoot: false, uptimeS: 10, now: 1000) == false, "no reset")
    check(r.status(resetAtBoot: true, uptimeS: 15, now: 1005) == true, "the flag appears later on the same boot: acted on now")
}

// The gear ratio field (openspec `app/settings`, «Редуктор — текст, проверенный до отправки»):
// a keystroke is judged, not written; the number goes to the car when the input is over.
// Typing «12.5» used to write 1.0, 12.0 and 12.5 in turn (AJM-112).
do {
    var g = GearEntry(ratio: 9)
    check(g.text == "9.00", "the car's ratio is shown with two decimals")
    g.typed("1"); check(g.valid, "1 is a ratio the car takes")
    g.typed("12"); check(g.valid, "12 too")
    g.typed("12."); check(g.valid, "12. reads as 12")
    g.typed("12.5"); check(g.valid, "12.5 too")
    check(g.finished() == 12.5, "the input over: one number, the last one")
    check(g.text == "12.50", "and the field shows it as the car will")
    check(g.finished() == nil, "finishing again writes nothing: the number is already the car's")
}

// A comma is a point.
do {
    var g = GearEntry(ratio: 9)
    g.typed("9,5"); check(g.valid, "9,5 is a number")
    check(g.finished() == 9.5, "and it is 9.5")
}

// Out of range or unreadable: never sent, and the field goes back to the car's number.
do {
    var g = GearEntry(ratio: 9)
    g.typed("400"); check(!g.valid, "above max: not a value the car takes")
    check(g.finished() == nil, "not sent")
    check(g.text == "9.00", "the field returns to the car's ratio")
    g.typed(""); check(!g.valid, "empty: not a number")
    check(g.finished() == nil && g.text == "9.00", "not sent, field restored")
    g.typed("abc"); check(!g.valid, "letters: not a number")
    check(g.finished() == nil && g.text == "9.00", "not sent, field restored")
    g.typed("0.5"); check(!g.valid, "below min")
    check(g.finished() == nil && g.text == "9.00", "not sent, field restored")
}

// Rounded to the field's step (1/100), half away from zero, before the compare and the write.
do {
    var g = GearEntry(ratio: 9)
    g.typed("9.555"); check(g.finished() == 9.56, "9.555 → 9.56")
    g.typed("9.56"); check(g.finished() == nil, "the same number typed again is not a write")
    g.typed("9.004"); check(g.finished() == 9.0, "9.004 rounds to 9.00 — a change from 9.56")
}

// The car answered with a ratio of its own (a read, «Прочитать снова»): the field takes it,
// which is not a write.
do {
    var g = GearEntry(ratio: 9)
    g.typed("12");
    g.adopt(11.25)
    check(g.text == "11.25", "the car's number replaces the draft")
    check(g.finished() == nil, "adopting is never a write")
}

if failures == 0 { print("test_configstate: OK") } else { exit(1) }
