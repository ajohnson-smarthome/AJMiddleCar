import Foundation

/// A configuration domain: one member of `/config`, generated from `contract/car-api.json`.
protocol ConfigDomain: Codable, Equatable, Sendable {
    static var key: String { get }
    static var `default`: Self { get }
    /// This domain out of a whole `/config` document, nil when the car did not send it.
    static func pick(from: CarConfig) -> Self?
    /// A `/config` body carrying only this domain — what a POST sends.
    static func wrap(_ v: Self) -> CarConfig
}

extension Ramp: ConfigDomain {}
extension Trim: ConfigDomain {}
extension Recovery: ConfigDomain {}
extension Wheel: ConfigDomain {}
extension Chassis: ConfigDomain {}
extension Video: ConfigDomain {}

/// What the app knows about one domain, and the transitions between those states.
///
/// The distinction `.unknown` makes is the whole point: a failed GET used to be indistinguishable
/// from real data, so `WheelParamsView` drew its hardcoded 65/11/2100/4 as if it were the car's
/// configuration, and one stepper tap POSTed the whole record — overwriting the car's real gear
/// ratio with the app's fallback.
///
/// The transitions are pure and host-tested; `ConfigDomainStore` is the shell that performs the
/// I/O between them.
enum ConfigState<T: ConfigDomain>: Equatable {
    /// Never read, or the read failed. Renders as "not read" — never as a value.
    case unknown
    case loaded(T)
    case saving(T)
    case failed(CarError)

    /// The car's value, or nil when we have not read one. A view that renders this as a number
    /// must render nil as "not read", not as a default.
    var value: T? {
        switch self {
        case .loaded(let v), .saving(let v): return v
        case .unknown, .failed: return nil
        }
    }

    var error: CarError? { if case .failed(let e) = self { return e }; return nil }

    /// A read finished.
    static func afterLoad(_ result: Result<T, CarError>) -> ConfigState {
        switch result {
        case .success(let v): return .loaded(v)
        case .failure(let e): return .failed(e)
        }
    }

    /// A write was asked for. `nil` means refused, for one of two reasons: the value is already
    /// the car's (no request, no NVS wear), or we never read the car's value — and a value we
    /// never read must not be written back over one we did not see.
    func afterSaveRequest(_ v: T) -> ConfigState? {
        guard let current = value else { return nil }
        guard current != v else { return nil }
        return .saving(v)
    }

    /// A write finished. Success keeps the value we sent, because the car took it.
    static func afterSave(_ result: Result<T, CarError>) -> ConfigState {
        switch result {
        case .success(let v): return .loaded(v)
        case .failure(let e): return .failed(e)
        }
    }
}

/// `storage.reset_at_boot`, read into one event per car boot.
///
/// The flag is true for the whole boot after an NVS format migration erased the saved
/// configuration (`docs/protocol.md` → Status), and the caches are then whatever the app
/// read from the car *before* — the previous boot's settings, shown as if the car still held
/// them, until the app was restarted (AJM-99). The first reading of the flag in a boot drops
/// every cache and tells the user once; a later reading from the same boot — a session
/// reopened, the firmware screen re-asking `/status` — is not a second reset, because what
/// was read after the drop is that boot's truth and pulling it from under an open screen
/// would only serve a stale answer twice.
///
/// A boot is told apart by its instant on the app's clock, `now − uptime_s`: the uptime
/// alone does not do, since a session reopened after a reboot can well read a larger uptime
/// than the previous boot's last reading. Two readings of one boot land within a few seconds
/// of each other on that scale (integer uptime, a request in flight, a retry); two boots are
/// a reboot apart, and a second migration is an OTA apart. Pure and host-tested; the store
/// is the shell that performs the drop.
struct StorageReset: Equatable {
    /// How far apart two readings of one boot may put its instant.
    static let sameBoot: TimeInterval = 5
    /// The boot already acted on, as its instant on the app's clock.
    private(set) var actedOn: TimeInterval?

    /// One `/status` reading: true when the caches must be dropped and the user told.
    mutating func status(resetAtBoot: Bool, uptimeS: Int, now: TimeInterval) -> Bool {
        guard resetAtBoot else { return false }
        let boot = now - TimeInterval(uptimeS)
        if let at = actedOn, abs(boot - at) <= Self.sameBoot { return false }
        actedOn = boot
        return true
    }
}

/// The gear-ratio field: text in, one number out — when the input is over, not per keystroke.
///
/// Typing «12.5» used to write 1.0, 12.0 and 12.5 in turn — three NVS commits for one number,
/// two of them wrong on the car for as long as the finger paused (AJM-112). A keystroke is
/// now only judged (`valid`, for the field's tint); the write is `finished()` — the keyboard's
/// «Готово», the focus leaving, the screen leaving — and it is one number, the last one, or
/// nothing when the text means nothing the car would take, in which case the field goes back
/// to the car's ratio rather than leave a number on screen that was never sent.
struct GearEntry: Equatable {
    /// `wheel.gear_ratio`'s `scale` in the contract: the field's step is 1/100.
    private static let scale = 100.0

    /// What the field shows.
    private(set) var text: String
    /// The car's ratio, as last read or written — what `finished()` compares against.
    private(set) var ratio: Double

    init(ratio: Double) {
        self.ratio = ratio
        text = Self.string(ratio)
    }

    /// The number the text means for the car, nil when it means nothing the car would take:
    /// unreadable, or outside `wheel.gear_ratio`'s range. A comma is a point; the result is
    /// rounded to the field's step, half away from zero, as the car holds it.
    static func value(_ text: String) -> Double? {
        guard let g = Double(text.replacingOccurrences(of: ",", with: ".")) else { return nil }
        guard Wheel.gear_ratioRange.contains(g) else { return nil }   // the car rejects, so don't ask
        return (g * scale).rounded() / scale
    }

    /// A ratio as the field shows it, two decimals.
    static func string(_ ratio: Double) -> String { String(format: "%.2f", ratio) }

    var valid: Bool { Self.value(text) != nil }

    /// A keystroke: the text moves, nothing is written.
    mutating func typed(_ text: String) { self.text = text }

    /// The car's ratio landed (a read, an answer to a write): the field takes it. Not a write.
    mutating func adopt(_ ratio: Double) {
        self.ratio = ratio
        text = Self.string(ratio)
    }

    /// The input is over: the ratio to write, or nil — the number is already the car's, or
    /// the text meant nothing and the field is back on the car's ratio.
    mutating func finished() -> Double? {
        guard let g = Self.value(text) else { text = Self.string(ratio); return nil }
        text = Self.string(g)
        guard g != ratio else { return nil }
        ratio = g
        return g
    }
}
