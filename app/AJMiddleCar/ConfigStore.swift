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
            let cfg = try JSONDecoder().decode(CarConfig.self, from: try await transport.get(CarContract.configPath))
            guard let v = T.pick(from: cfg) else { return .failure(.malformed("\(T.key) missing from /config")) }
            return .success(v)
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

    /// The car said its store was wiped at this boot (`ConfigStore.status`): what was read
    /// is no longer what it holds. Back to «не прочитано» — the screens re-read on their next
    /// opening, the prefetch re-reads its three. A read or a write already in flight lands
    /// afterwards as usual; it is answered by the same wiped car, so it is that boot's truth.
    func forget() { state = .unknown }

    #if DEBUG
    /// Gallery only: pretend the car answered, so a settings screen can be eyeballed without
    /// one. Nothing outside the debug gallery may write this state without a real read.
    func seed(_ v: T) { state = .loaded(v) }
    /// Gallery only: the same screen with the read never having landed.
    func seedUnknown() { state = .unknown }
    #endif

    private func write(_ v: T) async -> Result<T, CarError> {
        do {
            // The car answers a POST with the whole configuration as now held, so what is kept
            // is what the car has, not what was sent.
            let data = try await transport.post(CarContract.configPath, body: try JSONEncoder().encode(T.wrap(v)))
            let cfg = try JSONDecoder().decode(CarConfig.self, from: data)
            return .success(T.pick(from: cfg) ?? v)
        } catch let e as CarError {
            return .failure(e)
        } catch {
            return .failure(.malformed(String(describing: error)))
        }
    }
}

/// The six config domains, one instance each. Shared because the cache is read from places that
/// must not perform I/O — pressing a trick builds its geometry from `wheel` and `dims` here.
@MainActor
final class ConfigStore: ObservableObject {
    static let shared = ConfigStore()

    let ramp = ConfigDomainStore<Ramp>()
    let trim = ConfigDomainStore<Trim>()
    let recovery = ConfigDomainStore<Recovery>()
    let wheel = ConfigDomainStore<Wheel>()
    let chassis = ConfigDomainStore<Chassis>()
    let video = ConfigDomainStore<Video>()

    /// The car's settings and calibration were reset at its boot, and the user has not yet
    /// been told. The drive screen shows it until tapped away; once per car boot
    /// (`StorageReset`), whatever the session does in between.
    @Published private(set) var resetNotice = false
    private var storageReset = StorageReset()

    /// `/status` as read when the session opens (`CarLink`). `storage.reset_at_boot` is the
    /// one signal that the six caches are stale — the car's store was wiped, and what the app
    /// read before is the previous boot's settings (AJM-99). The first reading in a boot
    /// drops all six and raises the notice; the prefetch then re-reads its three, since the
    /// reading may have landed after the session's own prefetch already filled them.
    func status(_ s: CarStatus, now: TimeInterval) {
        guard storageReset.status(resetAtBoot: s.storage.reset_at_boot,
                                  uptimeS: s.system.uptime_s, now: now) else { return }
        ramp.forget(); trim.forget(); recovery.forget()
        wheel.forget(); chassis.forget(); video.forget()
        resetNotice = true
        prefetchDriveGeometry()
    }

    /// The user saw the notice.
    func dismissResetNotice() { resetNotice = false }

    /// Warm the domains the drive screen needs before the user can press anything that
    /// depends on them — the video switch, so the drive screen knows which layout to draw
    /// before it appears, and the two the tricks compute with.
    ///
    /// One task, in this order, not three: the transport serialises requests on a path, so
    /// three tasks made `video` the third `GET /config` round trip through the relay, and on
    /// a cold cache the drive screen — drawn with the first telemetry frame, within 200 ms
    /// of the handshake — appeared classic and flipped to the HUD when the third answer
    /// landed (AJM-114). `video` first is what the spec asks of the prefetch; the tricks'
    /// geometry is not needed until a tap.
    func prefetchDriveGeometry() {
        Task {
            await video.loadIfNeeded()
            await wheel.loadIfNeeded()
            await chassis.loadIfNeeded()
        }
    }
}
