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
