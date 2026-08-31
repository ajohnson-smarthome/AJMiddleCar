import Foundation

/// Fetches the latest firmware from GitHub Releases and uploads it to the car's /ota.
@MainActor
final class UpdateClient: NSObject, ObservableObject {
    struct Release { let tag: String; let assetURL: URL }
    @Published var uploadProgress: Double = 0
    @Published var downloadProgress: Double = 0

    /// Minimum on-screen duration for the download phase — also the synthetic fill
    /// time for DownloadBar, so the bar reaches 100% just as the screen advances.
    static let downloadMinDisplay: Double = 1.2

    /// Sleep for whatever remains of `seconds` since `start` (no-op if already elapsed).
    static func holdAtLeast(_ seconds: Double, since start: Date) async {
        let remaining = seconds - Date().timeIntervalSince(start)
        if remaining > 0 { try? await Task.sleep(nanoseconds: UInt64(remaining * 1_000_000_000)) }
    }

    private let repo = "ajohnson-smarthome/AJMiddleCar"
    /// Exact asset name for `device` (`UpdateRules.Device.assetName`). A release has carried
    /// both `ajmiddlecar.bin` and `ajdongle.bin` under one tag since branch P3
    /// (`tools/release.sh`); matching "first file ending in .bin" would silently hand back
    /// whichever the GitHub API happened to list first. That is the live situation on every
    /// release now, not a hazard being guarded against.
    static func assetName(for device: UpdateRules.Device) -> String { device.assetName }

    /// Normalize a version like "v1.2" / "v1.2-3-gabc" → "1.2" for comparison.
    static func normalize(_ v: String?) -> String { UpdateRules.normalize(v) }

    /// Build number after the first "+" (e.g. "v1.2+246" -> 246); nil if absent/non-numeric.
    static func buildNumber(_ version: String?) -> Int? { UpdateRules.buildNumber(version) }

    /// Update available iff both versions carry a build number and latest > running.
    /// Falls back to normalized string inequality when a build number is missing (legacy firmware/releases).
    static func isUpdateAvailable(running: String?, latest: String?) -> Bool {
        UpdateRules.isUpdateAvailable(running: running, latest: latest)
    }

    /// Need to (re)download the .bin: only when there IS a versioned latest release, and the
    /// cached file is missing or its build differs from the latest.
    static func needsDownload(latestBuild: Int?, cachedBuild: Int?, hasCachedFile: Bool) -> Bool {
        UpdateRules.needsDownload(latestBuild: latestBuild, cachedBuild: cachedBuild,
                                  hasCachedFile: hasCachedFile)
    }

    /// Forced update required iff the latest release carries a build number AND either the running
    /// firmware predates versioning (no build number) or its build is lower.
    static func mustUpdate(carFw: String?, latestTag: String?) -> Bool {
        UpdateRules.mustUpdate(carFw: carFw, latestTag: latestTag)
    }

    // MARK: - Internet reachability + firmware cache

    /// Lightweight reachability probe to GitHub (distinguishes "no internet" from "API failed").
    static func internetReachable() async -> Bool {
        guard let url = URL(string: "https://api.github.com") else { return false }
        // Waits for a usable path rather than failing on the first one it is handed.
        //
        // This is no longer defending against the car's own Wi-Fi. Joined to the car's access
        // point, iOS used to keep Wi-Fi in the general path for about forty seconds before
        // deciding it had no internet and demoting it — measured on the bench, see
        // docs/superpowers/specs/2026-08-21-wifi-pinned-networking.md — so a fast one-shot
        // probe launched inside that window went out over the car's dead Wi-Fi, timed out, and
        // dead-ended the launch gate on a phone whose cellular data was working the whole time.
        // With the dongle, that scenario cannot happen: the phone never joins the car's access
        // point at all, so its own Wi-Fi, with a real route to the internet, stays in the
        // general path throughout (`CarNet.swift`). What is left to wait out is ordinary: a
        // cold launch's network establishment (association, DNS) can still cost a couple of
        // seconds on a phone that is genuinely online, and a probe that gives up too fast reads
        // that as "no internet" for nothing.
        //
        // The 10 s / 25 s below are carried over unchanged from the old measurement and were
        // never re-sized against "a couple of seconds" — they are not tuned to today's scenario,
        // they are a safe ceiling for it: this only ever delays declaring "no internet" while a
        // path comes up, never causes a false one, so an oversized budget costs nothing but a
        // slower failure on a phone that truly has none.
        let cfg = URLSessionConfiguration.ephemeral
        cfg.waitsForConnectivity = true
        cfg.timeoutIntervalForRequest = 10
        cfg.timeoutIntervalForResource = 25
        let session = URLSession(configuration: cfg)
        defer { session.finishTasksAndInvalidate() }

        var req = URLRequest(url: url); req.httpMethod = "HEAD"
        if let (_, resp) = try? await session.data(for: req) {
            return (resp as? HTTPURLResponse) != nil
        }
        return false
    }

    /// Storage keys, per device — suffixed with `UpdateRules.Device.rawValue` so caching one
    /// device's build/tag can never be read back, or overwrite, the other's. The bare,
    /// unsuffixed prefixes are the keys every build before that split wrote, which is why they
    /// are named here rather than spelled again inside the migration below.
    private static let kBuildLegacy = "cachedLatestBuild"
    private static let kTagLegacy = "cachedLatestTag"
    private static func kBuild(_ device: UpdateRules.Device) -> String {
        "\(kBuildLegacy)-\(device.rawValue)"
    }
    private static func kTag(_ device: UpdateRules.Device) -> String {
        "\(kTagLegacy)-\(device.rawValue)"
    }

    /// Application Support, not Caches: this file is the offline gate's lifeline (GateRule),
    /// and iOS may purge Caches under storage pressure — evaporating the one thing that lets
    /// a phone in the field proceed without internet. Excluded from backup: it is a cache in
    /// spirit, just not one the OS may unilaterally delete.
    ///
    /// One path per device — `UpdateRules.cacheURL(for:in:)` is the pure join that guarantees
    /// it, host-tested there; this only supplies (and creates) the real directory. A stale car
    /// image can never be found — let alone offered to `DongleClient.uploadFirmware` — under the
    /// dongle's path, or vice versa: the two devices simply never share a file.
    static func cachedBinURL(for device: UpdateRules.Device) -> URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory,
                                           in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return UpdateRules.cacheURL(for: device, in: dir)
    }
    /// The car's cache — the only device this existed for before branch P4's dongle images.
    static var cachedBinURL: URL { cachedBinURL(for: .car) }

    /// One-time move of a pre-existing cache from wherever an older build left it, plus the
    /// keys that describe it. Called at launch; a no-op when there is nothing to migrate.
    /// Car-only: the undifferentiated `firmware-latest.bin` could only ever have been the
    /// car's — the dongle's own cache did not exist before this per-device split.
    ///
    /// BOTH old locations, which is the fix. This knew only `Caches/firmware-latest.bin` while
    /// the per-device rename moved the car's file out from under Application
    /// Support/`firmware-latest.bin` — where every phone that had already migrated once was
    /// holding it. That file went invisible, and `GateRule` reads the consequences: no cached
    /// file means `canProceedOffline` is false, so a launch with no internet lands on
    /// `.noInternet` where it used to proceed, and `latestTag` stays nil, so the forced update
    /// gate silently never fires. Those are the two failures that decision exists to prevent,
    /// and nothing shows them until someone launches offline.
    static func migrateCacheIfNeeded() {
        let fm = FileManager.default
        let new = cachedBinURL              // also creates Application Support, if it is new
        let legacy = UpdateRules.legacyCacheURLs(
            caches: fm.urls(for: .cachesDirectory, in: .userDomainMask)[0],
            appSupport: fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0])
        for old in legacy where fm.fileExists(atPath: old.path) {
            guard !fm.fileExists(atPath: new.path) else {
                // The image is already under its per-device name, so anything left under the
                // old one can never be read again — and it is the better part of a megabyte.
                try? fm.removeItem(at: old)
                continue
            }
            try? fm.moveItem(at: old, to: new)
            excludeFromBackup(new)
        }
        migrateCacheKeysIfNeeded()
    }

    /// The other half of that rename: the build and tag the cached file is described BY. A file
    /// without them is not a usable cache — `GateRule.canProceedOffline` wants both — so moving
    /// one without the other would have fixed nothing.
    ///
    /// Mirrored rather than moved, and only where the per-device key is unset: an older build
    /// of this app run again on the same phone still reads the unsuffixed keys, and a newer
    /// download that has already recorded its own must never be overwritten by an older value.
    private static func migrateCacheKeysIfNeeded() {
        let d = UserDefaults.standard
        if d.object(forKey: kBuild(.car)) == nil,
           let legacy = d.object(forKey: kBuildLegacy) as? Int, legacy != 0 {
            d.set(legacy, forKey: kBuild(.car))
        }
        if d.string(forKey: kTag(.car)) == nil, let legacy = d.string(forKey: kTagLegacy) {
            d.set(legacy, forKey: kTag(.car))
        }
    }

    private static func excludeFromBackup(_ url: URL) {
        var u = url
        var rv = URLResourceValues()
        rv.isExcludedFromBackup = true
        try? u.setResourceValues(rv)
    }
    static func cachedBuild(for device: UpdateRules.Device) -> Int? {
        let v = UserDefaults.standard.integer(forKey: kBuild(device)); return v == 0 ? nil : v
    }
    static var cachedBuild: Int? { cachedBuild(for: .car) }

    static func cachedTag(for device: UpdateRules.Device) -> String? {
        UserDefaults.standard.string(forKey: kTag(device))
    }
    static var cachedTag: String? { cachedTag(for: .car) }

    static func hasCachedFile(for device: UpdateRules.Device) -> Bool {
        FileManager.default.fileExists(atPath: cachedBinURL(for: device).path)
    }
    static var hasCachedFile: Bool { hasCachedFile(for: .car) }

    static func recordCache(build: Int, tag: String, for device: UpdateRules.Device = .car) {
        UserDefaults.standard.set(build, forKey: kBuild(device))
        UserDefaults.standard.set(tag, forKey: kTag(device))
    }

    /// Defaults to the car — the only device this asked about before branch P4's dongle images.
    /// One release carries both under the same tag, so a caller after the dongle's own asset
    /// passes `device: .dongle` and gets the same tag back with a different `assetURL`.
    /// What a release lookup can come back as.
    ///
    /// These used to be one `nil`, and collapsing them was expensive the day the launch gate
    /// became strict: the newest release carried only the car's image, so the adapter's lookup
    /// returned nothing forever, the gate refused to hand over — correctly — and the screen said
    /// "no internet" at a phone whose internet was fine. Unreachable is the user's problem to
    /// fix; a release with no image for this device is nobody's, until one is published.
    enum ReleaseLookup {
        case found(Release)
        /// A release exists and was read, and it carries no image for this device.
        case noImage(tag: String)
        /// GitHub could not be reached, or answered something this build cannot read.
        case unreachable
    }

    func latestReleaseLookup(for device: UpdateRules.Device = .car) async -> ReleaseLookup {
        guard let url = URL(string: "https://api.github.com/repos/\(repo)/releases/latest") else {
            return .unreachable
        }
        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let tag = j["tag_name"] as? String,
                  let assets = j["assets"] as? [[String: Any]] else { return .unreachable }
            let bin = assets.first { ($0["name"] as? String) == UpdateClient.assetName(for: device) }
            guard let s = bin?["browser_download_url"] as? String, let u = URL(string: s) else {
                return .noImage(tag: tag)
            }
            return .found(Release(tag: tag, assetURL: u))
        } catch { return .unreachable }
    }

    func latestRelease(for device: UpdateRules.Device = .car) async -> Release? {
        if case .found(let r) = await latestReleaseLookup(for: device) { return r }
        return nil
    }

    /// Decision 6: nothing enters the firmware cache unvalidated. URLSession does not throw
    /// on 404/403/5xx, so an error page used to be cached as firmware and recorded as the
    /// latest build — poisoning the cache until a strictly newer release existed. A failed
    /// download never installs an invalid image; a failed move can drop the old cache (the
    /// remove happens before the move), which every consumer tolerates by re-checking
    /// `hasCachedFile` live rather than trusting a cached boolean.
    ///
    /// Defaults to the car; a caller downloading the dongle's image passes `device: .dongle` and
    /// lands in `cachedBinURL(for: .dongle)` — a different file, under a different recorded
    /// build/tag, than whatever the car has cached.
    func download(_ url: URL, recordAs: (build: Int, tag: String)? = nil,
                  device: UpdateRules.Device = .car) async -> URL? {
        downloadProgress = 0
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        do {
            let (tmp, resp) = try await session.download(from: url)
            guard (resp as? HTTPURLResponse)?.statusCode == 200 else { return nil }
            let size = (try? FileManager.default.attributesOfItem(atPath: tmp.path)[.size]
                            as? Int) ?? 0
            let firstByte = FileHandle(forReadingAtPath: tmp.path).flatMap { fh -> UInt8? in
                defer { try? fh.close() }
                return try? fh.read(upToCount: 1)?.first
            }
            guard UpdateRules.isValidImage(firstByte: firstByte, size: size) else { return nil }
            let dest = UpdateClient.cachedBinURL(for: device)
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.moveItem(at: tmp, to: dest)
            UpdateClient.excludeFromBackup(dest)
            if let r = recordAs { UpdateClient.recordCache(build: r.build, tag: r.tag, for: device) }
            return dest
        } catch { return nil }
    }

    /// `failed`'s payload is the reason the CAR gave, when it gave one — only the
    /// `CarError.http` envelope branch below populates it. Every transport-level failure (no
    /// firmware file on disk, no dongle, timeout, refused, malformed/truncated stream) carries
    /// `nil`: the car never answered, so there is no car-authored reason to quote, and
    /// `fw.failReason`'s "Машинка ответила: …" framing would be a lie for those.
    enum UploadOutcome: Equatable { case ok, cancelled, failed(String?) }

    /// Uploads over `CarTransport`, pinned to the dongle's interface like every request to the
    /// car (`CarNet.tcpParams()`, which asks `CarInterface` which wire that is) — the phone
    /// reaches the car through the dongle, not over Wi-Fi. The wait scales with the image — see
    /// `UpdateRules.uploadTimeout`, and why a flat 45 s stopped being safe. The car's error envelope is
    /// surfaced, not swallowed (decision 14) — but only when it really is the car's own
    /// envelope; see `UploadOutcome`.
    func upload(_ binURL: URL) async -> UploadOutcome {
        uploadProgress = 0
        guard let data = try? Data(contentsOf: binURL) else { return .failed(nil) }
        do {
            _ = try await CarTransport.shared.post("/ota", body: data,
                                                   contentType: "application/octet-stream",
                                                   timeout: UpdateRules.uploadTimeout(bytes: data.count)) { [weak self] p in
                Task { @MainActor in self?.uploadProgress = p }
            }
            return .ok
        } catch is CancellationError {
            return .cancelled
        } catch let CarError.http(status, body) {
            let msg = ((try? JSONSerialization.jsonObject(with: body)) as? [String: Any])?["error"] as? String
            return .failed(msg ?? "HTTP \(status)")
        } catch {
            // `CarError` (`.noDongle`, `.denied`, `.refused`, `.timeout`, `.malformed`,
            // `.truncated`) or anything else unexpected: none of these are the car speaking,
            // they're the transport never reaching it — generic copy, not a fabricated quote.
            // Nothing on screen names the reason, so it goes to the log instead.
            print("upload failed: \((error as? CarError)?.logDescription ?? String(describing: error))")
            return .failed(nil)
        }
    }
}

extension UpdateClient: URLSessionTaskDelegate, URLSessionDownloadDelegate {
    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask,
                                didSendBodyData bytesSent: Int64, totalBytesSent: Int64,
                                totalBytesExpectedToSend: Int64) {
        let p = totalBytesExpectedToSend > 0 ? Double(totalBytesSent) / Double(totalBytesExpectedToSend) : 0
        Task { @MainActor in self.uploadProgress = p }
    }
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                                didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                                totalBytesExpectedToWrite: Int64) {
        let p = totalBytesExpectedToWrite > 0 ? Double(totalBytesWritten) / Double(totalBytesExpectedToWrite) : 0
        Task { @MainActor in self.downloadProgress = p }
    }
    // Required by URLSessionDownloadDelegate; async download(from:) consumes the file itself.
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                                didFinishDownloadingTo location: URL) { }
}
