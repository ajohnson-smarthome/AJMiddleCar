import Foundation

/// One domain's state machine over the transport. Generic, so the five hand-written clients that
/// each flattened every failure to `nil` are one implementation.
@MainActor
final class ConfigDomainStore<T: ConfigDomain>: ObservableObject {
    @Published private(set) var state: ConfigState<T> = .unknown

    private let transport: CarTransport
    private var loading = false

    init(transport: CarTransport = .shared) { self.transport = transport }

    var value: T? { state.value }
    var error: CarError? { state.error }
    var isBusy: Bool { if case .saving = state { return true }; return loading }

    /// Read once per session's worth of screen visits; `reload()` forces it.
    func loadIfNeeded() async {
        if case .loaded = state { return }
        await reload()
    }

    func reload() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        state = .afterLoad(await read())
    }

    private func read() async -> Result<T, CarError> {
        do {
            return .success(try JSONDecoder().decode(T.self, from: try await transport.get(T.path)))
        } catch let e as CarError {
            return .failure(e)
        } catch {
            return .failure(.malformed(String(describing: error)))
        }
    }

    /// Write a value back. Refused while the car's own value is unknown — that refusal is the
    /// fix: without it, the first tap on a control the user can see writes the app's fallback
    /// over configuration it never managed to read.
    @discardableResult
    func save(_ v: T) async -> Bool {
        guard let next = state.afterSaveRequest(v) else { return value == v }
        state = next
        state = .afterSave(await write(v))
        return state.value == v && state.error == nil
    }

    #if DEBUG
    /// Gallery only: pretend the car answered, so a settings screen can be eyeballed without
    /// one. Nothing outside the debug gallery may write this state without a real read.
    func seed(_ v: T) { state = .loaded(v) }
    /// Gallery only: the same screen with the read never having landed.
    func seedUnknown() { state = .unknown }
    #endif

    private func write(_ v: T) async -> Result<T, CarError> {
        do {
            _ = try await transport.post(T.path, body: try JSONEncoder().encode(v))
            return .success(v)
        } catch let e as CarError {
            return .failure(e)
        } catch {
            return .failure(.malformed(String(describing: error)))
        }
    }
}

/// The five config domains, one instance each. Shared because the cache is read from places that
/// must not perform I/O — pressing a trick builds its geometry from `wheel` and `dims` here.
@MainActor
final class ConfigStore {
    static let shared = ConfigStore()

    let ramp = ConfigDomainStore<Ramp>()
    let trim = ConfigDomainStore<Trim>()
    let recover = ConfigDomainStore<Recover>()
    let wheel = ConfigDomainStore<Wheel>()
    let dims = ConfigDomainStore<Dims>()

    /// Warm the two domains the drive screen needs before the user can press anything that
    /// depends on them.
    func prefetchDriveGeometry() {
        Task { await wheel.loadIfNeeded() }
        Task { await dims.loadIfNeeded() }
    }
}
