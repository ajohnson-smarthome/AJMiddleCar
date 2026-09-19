// Host test for FlashRefusal — what the firmware screen says when a flash did not end in `ok`,
// for the car and the adapter alike. The phrases are read from the app's own strings table, so
// this is «ошибка клиента → текст отказа» end to end, not a check of keys. Run with swiftc.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

// -- the strings table, as the app ships it -----------------------------------------------------
// `#filePath` is app/tests/flashrefusal/main.swift; the table sits three levels up, whatever the
// working directory tools/test-all.sh happens to run this from.
let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
let tablePath = root.appendingPathComponent("app/AJMiddleCar/Resources/ru.lproj/Localizable.strings")
let table: [String: String] = {
    guard let data = try? Data(contentsOf: tablePath),
          let obj = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
          let d = obj as? [String: String] else {
        print("FAIL: cannot read the strings table at \(tablePath.path)"); exit(1)
    }
    return d
}()
/// The table's phrase for a code — what `L.fwFailLine` asks for. A code the table lacks is
/// reported loudly rather than shown as the key, which is what NSLocalizedString would do.
func phrase(_ code: String) -> String { table["err." + code] ?? "<no phrase for \(code)>" }
/// The line the screen renders: `fw.failReason.<device>` around the quote, or `fw.failSub`.
func line(_ device: String, _ r: FlashRefusal) -> String {
    guard let q = r.quote(phrase: phrase) else { return table["fw.failSub"]! }
    return String(format: table["fw.failReason.\(device)"]!, q)
}

func envelope(_ code: String, _ message: String, field: String? = nil, proto: Int = 2) -> Data {
    var e: [String: Any] = ["code": code, "message": message]
    if let field { e["field"] = field }
    return try! JSONSerialization.data(withJSONObject: ["proto": proto, "error": e])
}

// -- the car rejected the image: the code's phrase, never the message (AJM-57) ------------------
let notFirmware = "image is not an application image"
let carRefused = FlashRefusal.of(CarError.http(status: 400, body: envelope("not_firmware", notFirmware)))
check(carRefused == .envelope(code: "not_firmware", message: notFirmware),
      "a 400 with the envelope is classified as the envelope, code and message apart")
let carLine = line("car", carRefused)
check(carLine == "Машинка ответила: " + phrase("not_firmware"),
      "the car's refusal is its code as the table's phrase: \(carLine)")
check(!carLine.contains(notFirmware), "the envelope's message is not on screen: \(carLine)")
check(!carLine.contains("not_firmware"), "a known code is shown as its phrase, not as the word")
check(carRefused.logDescription.contains("not_firmware") && carRefused.logDescription.contains(notFirmware),
      "code and message both go to the log: \(carRefused.logDescription)")

// -- the adapter rejected the image: same rule, its own codes (AJM-58) --------------------------
let dongleRefused = FlashRefusal.of(CarError.http(status: 400, body: envelope("too_small", "image too small", proto: 1)))
check(line("dongle", dongleRefused) == "Адаптер ответил: " + phrase("too_small"),
      "the adapter's refusal is its code as the table's phrase")
let dongleJoin = FlashRefusal.of(CarError.http(status: 400, body: envelope("bad_length", "ssid must be 1..32 bytes", field: "ssid", proto: 1)))
check(dongleJoin.quote(phrase: phrase) == phrase("bad_length"),
      "a code only the adapter's contract has is a phrase too")

// -- one phrase per code of both contracts -----------------------------------------------------
let carCodes = CarErrorCode.all.map(\.rawValue)
let dongleCodes = DongleErrorCode.all.map(\.rawValue)
for code in Set(carCodes + dongleCodes) {
    let p = table["err." + code]
    check(p != nil && !p!.isEmpty, "the table has a phrase for \(code)")
    check(p != code, "the phrase for \(code) is words, not the code again")
    check(FlashRefusal.knownCodes.contains(code), "\(code) is a known code")
    let r = FlashRefusal.of(CarError.http(status: 400, body: envelope(code, "whatever the board wrote")))
    check(r.quote(phrase: phrase) == p, "\(code) is quoted as its phrase")
}
check(FlashRefusal.knownCodes == Set(carCodes + dongleCodes),
      "the known codes are exactly the two contracts' lists — nothing invented, nothing missed")

// -- a code this build does not know is shown as the word itself --------------------------------
let novel = FlashRefusal.of(CarError.http(status: 400, body: envelope("quantum_flux", "reticulating splines")))
check(novel == .envelope(code: "quantum_flux", message: "reticulating splines"), "an unknown code is still the envelope")
check(novel.quote(phrase: phrase) == "quantum_flux", "an unknown code is quoted as the word the board said")
check(line("car", novel) == "Машинка ответила: quantum_flux", "…under the same caption")

// -- an envelope short of its message still names its code --------------------------------------
let terse = try! JSONSerialization.data(withJSONObject: ["proto": 2, "error": ["code": "busy"]])
check(FlashRefusal.of(CarError.http(status: 409, body: terse)) == .envelope(code: "busy", message: ""),
      "a message-less envelope is still classified by its code")

// -- an HTTP error without the envelope names the status ----------------------------------------
let html = FlashRefusal.of(CarError.http(status: 502, body: Data("<html>Bad Gateway</html>".utf8)))
check(html == .http(status: 502), "a body that is not the envelope is a bare HTTP error")
check(line("car", html) == "Машинка ответила: HTTP 502", "…quoted as its status")
let okFalse = try! JSONSerialization.data(withJSONObject: ["proto": 2, "ok": false])
check(FlashRefusal.of(CarError.http(status: 500, body: okFalse)) == .http(status: 500),
      "JSON that is not the envelope is a bare HTTP error too")
check(FlashRefusal.of(CarError.http(status: 404, body: Data())) == .http(status: 404), "an empty body as well")

// -- nothing answered: no quote, for either board (AJM-58) --------------------------------------
let transport: [(CarError, String)] = [
    (.timeout(60), "timeout"),
    (.noDongle(.notAvailable), "no dongle"),
    (.refused, "refused"),
    (.truncated(got: 3, want: 24), "truncated"),
    (.malformed("no parseable response head"), "malformed"),
    (.denied, "denied"),
]
for (e, name) in transport {
    let r = FlashRefusal.of(e)
    check(r == .transport(e.logDescription), "\(name) is transport, with the client's words for the log")
    check(r.quote(phrase: phrase) == nil, "\(name) is not quoted")
    check(line("car", r) == "Проверь связь и повтори", "\(name): the car gets the generic line")
    check(line("dongle", r) == "Проверь связь и повтори",
          "\(name): the adapter gets the same generic line, not «Адаптер ответил: \(name)»")
    check(!line("dongle", r).contains(String(describing: e)), "\(name): Swift's description of the error is not on screen")
}
struct Elsewhere: Error {}
check(FlashRefusal.of(Elsewhere()).quote(phrase: phrase) == nil, "an error that is not the client's is transport too")
check(FlashRefusal.of(Elsewhere()) == .transport("Elsewhere()"), "…with its description for the log")

if failures == 0 { print("test_flashrefusal: OK") } else { exit(1) }
