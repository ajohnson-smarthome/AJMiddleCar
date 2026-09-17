import Foundation
import Combine

/// The firmware update, as one piece of logic for both devices.
///
/// The car and the adapter used to update along completely different paths. The car had this
/// whole machine — check, offer, download, connect, upload, reboot, confirm — behind a screen
/// with progress and a cancel button. The adapter had `performDongleUpdate()`: a headless
/// function that downloaded, uploaded and slept through the reboot behind a single spinner, with
/// an attempt budget instead of a report. Same job, two implementations, and only one of them
/// could tell you what had gone wrong.
///
/// There is one now, and the two devices differ in exactly four things — what version the device
/// reports, whether it is answering, how a refresh of those two is obtained, and where the image
/// is pushed. Everything else, including every phase transition and every piece of copy, is
/// shared. That is the whole point: a person updating the adapter should be looking at the screen
/// they already know from updating the car.
@MainActor
final class FirmwareFlow: ObservableObject {
    let device: UpdateRules.Device
    @Published private(set) var phase: FwPhase = .checking
    @Published private(set) var release: UpdateClient.Release?
    /// Flashing an image already on disk, with no network involved.
    @Published private(set) var offlineCache = false
    /// The device came back running what it ran before — a bootloader rollback, never success.
    @Published private(set) var rolledBack = false
    /// The device's own words for why it refused the image, when it gave any.
    @Published private(set) var failReason: String?
    /// Whether an upload has been attempted this session. The rolled-back copy is only honest
    /// after one.
    @Published private(set) var flashAttempted = false
    @Published private(set) var downloadProgress: Double = 0
    @Published private(set) var uploadProgress: Double = 0

    let client = UpdateClient()

    // ── the seam: four questions, one verb ───────────────────────────────
    private let runningFw: @MainActor () -> String?
    private let isReachable: @MainActor () -> Bool
    /// Called on every tick of the reboot watch. Both boards are asked through `/version`: the
    /// car through the relay, the adapter over USB.
    private let refresh: @MainActor () async -> Void
    private let push: @MainActor (URL, UpdateClient, @escaping @MainActor (Double) -> Void) async -> UpdateClient.UploadOutcome

    private var binURL: URL?
    private var uploadTask: Task<Void, Never>?
    private var bag = Set<AnyCancellable>()

    init(device: UpdateRules.Device,
         runningFw: @escaping @MainActor () -> String?,
         isReachable: @escaping @MainActor () -> Bool,
         refresh: @escaping @MainActor () async -> Void = {},
         progressPublishedByClient: Bool = false,
         push: @escaping @MainActor (URL, UpdateClient, @escaping @MainActor (Double) -> Void) async -> UpdateClient.UploadOutcome) {
        self.device = device
        self.runningFw = runningFw
        self.isReachable = isReachable
        self.refresh = refresh
        self.push = push
        // The download half is the same client for both devices, so its progress is mirrored
        // rather than re-plumbed. The upload half differs, so it arrives through `push`.
        client.$downloadProgress.assign(to: &$downloadProgress)
        // The car's uploader publishes its own progress; the adapter's reports it through a
        // callback. One published value either way, so the screen never has to know which.
        if progressPublishedByClient { client.$uploadProgress.assign(to: &$uploadProgress) }
    }

    var currentFw: String? { runningFw() }
    var reachable: Bool { isReachable() }
    var canCancelUpload: Bool { uploadTask != nil }

    // ── check ────────────────────────────────────────────────────────────
    func check() async {
        phase = .checking
        // Ask the device where it stands before comparing anything against it. Both boards
        // answer from `/version`, and without this the first check would compare a release
        // against a version nobody had read yet.
        await refresh()
        offlineCache = false
        // A rollback from an earlier bounce must not decorate a later, unrelated failure with
        // rollback copy, and a previous upload's device-authored reason must not survive to
        // caption a different one. This is the only re-entry point, so it is where both are
        // cleared.
        rolledBack = false
        failReason = nil
        if let r = await client.latestRelease(for: device) {
            release = r
            phase = UpdateClient.isUpdateAvailable(running: runningFw(), latest: r.tag)
                ? .available : .upToDate
            return
        }
        release = nil
        // GitHub unreachable. A cached image strictly NEWER than what the device runs is
        // flashable with no network at all — and the device's own build has to be known, because
        // with nothing to compare against there is no "newer".
        if UpdateClient.hasCachedFile(for: device),
           let cached = UpdateClient.cachedBuild(for: device),
           let running = UpdateRules.buildNumber(runningFw()),
           cached > running {
            offlineCache = true
            binURL = UpdateClient.cachedBinURL(for: device)
            phase = .available
            return
        }
        phase = .failed
    }

    // ── download ─────────────────────────────────────────────────────────
    func download() async {
        if offlineCache {
            // Already on disk and validated when it was fetched; downloading it would be a
            // fetch of what we are standing on.
            phase = .downloaded
            return
        }
        guard let r = release else { return }
        // `flashPlan`, pure and host-tested: an image of exactly this build that is already on
        // disk — from an earlier forced flash, or the settings screen — is used, not fetched
        // again. Before this, a forced car update re-downloaded the 2 MB it already held and sat
        // on «Скачивание 100 %» for the whole second fetch.
        switch UpdateRules.flashPlan(for: device, release: (tag: r.tag, assetURL: r.assetURL),
                                     cachedBuild: UpdateClient.cachedBuild(for: device),
                                     hasCachedFile: UpdateClient.hasCachedFile(for: device)) {
        case .useCache:
            binURL = UpdateClient.cachedBinURL(for: device)
            phase = .downloaded
        case .download(_, let url, let build, let tag):
            phase = .downloading
            let t0 = Date()
            let recordAs = build.map { (build: $0, tag: tag) }
            if let got = await client.download(url, recordAs: recordAs, device: device) {
                binURL = got
                await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
                phase = .downloaded
            } else {
                phase = .failed
            }
        case .unavailable:
            phase = .failed
        }
    }

    /// Forced mode's answer to `.downloaded`: flash the moment the device is reachable, whether
    /// that is now or later. `FirmwareView` used to flash on the phase change only if the
    /// device happened to be reachable in that same instant; a car still re-joining through
    /// the dongle was not, and the screen then waited for a tap on a button nothing told the
    /// user to press. Returns when the phase has moved on, or when cancelled.
    ///
    /// The flash itself runs in a task of its own, NOT in this one. `FirmwareView` runs this
    /// from `.task(id: flow.phase)`, and `flash()`'s first act is to move the phase — which
    /// cancels the task it is running in. The upload survived that (it is a child `Task` of
    /// its own), but the reboot watch ran cancelled: every sleep returned at once, every
    /// `/version` threw before opening a connection, and thirty seconds of that ended as
    /// «Прошито» for an adapter that had come back within twenty (bench, 2026-09-15).
    func flashWhenReachable() async {
        while phase == .downloaded {
            await refresh()
            if isReachable() {
                Task { @MainActor in await self.flash() }
                return
            }
            try? await Task.sleep(for: .milliseconds(500))
            if Task.isCancelled { return }
        }
    }

    // ── flash ────────────────────────────────────────────────────────────
    func flash() async {
        guard let url = binURL else { return }
        flashAttempted = true
        rolledBack = false
        failReason = nil
        uploadProgress = 0
        phase = .uploading

        var outcome: UpdateClient.UploadOutcome = .cancelled
        let task = Task { @MainActor in
            outcome = await push(url, self.client) { [weak self] p in self?.uploadProgress = p }
        }
        uploadTask = task
        await task.value
        uploadTask = nil

        switch outcome {
        case .cancelled:
            // Back to flash-ready, not to failure: the user changed their mind, nothing broke.
            phase = .downloaded
            return
        case .failed(let reason):
            failReason = reason
            phase = .failed
            return
        case .ok:
            break
        }

        // Acknowledged: the image is written, set as the boot target, and the reboot follows.
        // From here the flash is COMMITTED — the only question left is whether this phone gets
        // to watch the confirmation.
        phase = .rebooting
        let oldFw = runningFw()
        var sawOffline = false
        let deadline = Date.now.addingTimeInterval(UpdateRules.rebootWindow(for: device))
        while Date.now < deadline {
            try? await Task.sleep(nanoseconds: 500_000_000)
            await refresh()
            // A version that differs from the one before the flash — including one appearing
            // where a 404 board had none — is the update landing.
            if let now = runningFw(), now != oldFw { phase = .done; return }
            if !isReachable() {
                sawOffline = true
            } else if sawOffline {
                // It came back running the SAME firmware. That is a bootloader rollback, or a
                // flash that never took — never success. Calling it done is what looped the
                // forced gate forever against a rolling-back release.
                rolledBack = true
                phase = .failed
                return
            }
        }
        // The deadline passed without a reconnect. The device's own reboot can outlast this
        // window; the flash is committed either way, so report that rather than a failure.
        phase = .flashed
    }

    func cancelUpload() { uploadTask?.cancel() }

    /// The gallery renders a frozen phase with no network behind it.
    func seed(_ phase: FwPhase) {
        self.phase = phase
        flashAttempted = true
    }
}


// MARK: - The two devices

extension FirmwareFlow {
    /// The car, reached through the relay: `/version` for what it runs and whether it answers,
    /// `POST /ota` for the image. No session is involved on either leg — which is what lets a
    /// car speaking an older protocol be updated at all.
    static func forCar() -> FirmwareFlow {
        let state = VersionState { try await CarTransport.shared.get(CarContract.versionPath, timeout: 2) }
        return FirmwareFlow(device: .car,
                            runningFw: { state.fw },
                            isReachable: { state.reachable },
                            refresh: { await state.refresh() },
                            progressPublishedByClient: true,
                            push: { url, client, _ in await client.upload(url) })
    }

    /// The adapter, reached over USB. Same shape; only the client differs.
    static func forDongle(client dongle: DongleClient) -> FirmwareFlow {
        let state = VersionState { try await dongle.versionData() }
        return FirmwareFlow(device: .dongle,
                            runningFw: { state.fw },
                            isReachable: { state.reachable },
                            refresh: { await state.refresh() },
                            push: { url, _, progress in
            guard let data = try? Data(contentsOf: url) else { return .failed(nil) }
            do {
                try await dongle.uploadFirmware(data) { p in
                    Task { @MainActor in progress(p) }
                }
                return .ok
            } catch is CancellationError {
                return .cancelled
            } catch {
                // The adapter's own words when it gave any; nil when nothing answered, which the
                // screen renders as the generic failure rather than quoting silence.
                return .failed((error as? CarError).map { String(describing: $0) })
            }
        })
    }
}

/// A board's last `/version` answer, refreshed on demand. A class rather than captured `var`s
/// because the flow's closures all have to see the same value, and because the reboot watch
/// reads it every 500 ms from a task that outlives whichever call started it.
@MainActor
private final class VersionState {
    private let read: () async throws -> Data
    var fw: String?
    var reachable = false
    init(read: @escaping () async throws -> Data) { self.read = read }

    func refresh() async {
        // One read of the frozen document, classified the way the gate classifies it.
        let reply: VersionReply
        do { reply = VersionReply.decode(try await read()) } catch { reply = VersionReply.of(error) }
        switch reply {
        case .version(let v):
            fw = v.fw
            reachable = true
        case .absent:
            // A board older than /version: it is there, it takes POST /ota, and it is exactly
            // the board the gate sent here to be updated. Only its version is unknown.
            fw = nil
            reachable = true
        case .silent, .faulty, .denied:
            // Not answering, or answering with something that is not the document — a reboot
            // in progress, or not our board at all. Not reachable for a flash either way.
            reachable = false
        }
    }
}
