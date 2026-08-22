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
let carsOwn = Wheel(diameter_mm: 70, ppr: 12, gear_x100: 960, quad: 2)
let loaded = ConfigState<Wheel>.afterLoad(.success(carsOwn))
check(loaded == .loaded(carsOwn) && loaded.value == carsOwn, "a read becomes the value")
check(ConfigState<Wheel>.afterLoad(.failure(.timeout(3))) == .failed(.timeout(3)), "a failed read")

// A write of the same value is not a write: no request, no NVS wear.
check(loaded.afterSaveRequest(carsOwn) == nil, "saving the same value is refused")

// A write of a different value goes through saving, and the value stays readable while it does.
let edited = Wheel(diameter_mm: 65, ppr: 12, gear_x100: 960, quad: 2)
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
check(Ramp.path == "/ramp" && Trim.path == "/trim" && Recover.path == "/recover"
      && Wheel.path == "/wheel" && Dims.path == "/dims", "the five domains")

if failures == 0 { print("test_configstate: OK") } else { exit(1) }
