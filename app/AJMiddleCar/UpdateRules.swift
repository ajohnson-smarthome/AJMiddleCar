import Foundation

/// The update chain's pure decisions, extracted from `UpdateClient` so they are host-tested.
/// `UpdateClient` keeps the sockets, the cache files and the sessions; this answers "which
/// version wins" and "what may enter the firmware cache".
enum UpdateRules {
    /// Which firmware image: the car's, or the dongle's. `tools/release.sh` has attached both
    /// `ajmiddlecar.bin` and `ajdongle.bin` to every release under one tag since branch P3 — one
    /// release, two images, one version between them. This is the only vocabulary that
    /// distinguishes them; `mustUpdate`, `isUpdateAvailable`, `needsDownload` and `isValidImage`
    /// below stay untouched because they only ever compare version strings or bytes, and neither
    /// cares whose they are.
    enum Device: String, CaseIterable {
        case car
        case dongle

        /// The release asset's exact name — the contract's own device string plus the firmware
        /// extension, so it is derived rather than hand-spelled here. A release has carried both
        /// images under one tag since branch P3 (`tools/release.sh`); matching "first file ending
        /// in .bin" would silently hand back whichever the GitHub API happened to list first.
        /// That is the live situation on every release now, not a hazard being guarded against.
        var assetName: String {
            switch self {
            case .car: return "\(CarContract.device).bin"
            case .dongle: return "\(DongleContract.device).bin"
            }
        }

        /// The local cache's filename — the asset name itself, not a second literal. A car image
        /// and a dongle image can only ever collide on disk if their asset names collide, and
        /// `assetName` above guarantees they never do: a stale image cached under one device's
        /// name can never be found, let alone offered, under the other's.
        var cacheFileName: String { assetName }
    }

    /// The one file name every build before the per-device split used for the car's cached
    /// image. Nothing writes it any more; it only has to be FOUND, in either of the two
    /// directories it has lived in.
    static let legacyCacheFileName = "firmware-latest.bin"

    /// Where an older build's car image can still be sitting, newest location first.
    ///
    /// Two, not one, and that is the whole point of this being a list with a test: the cache
    /// moved twice. First out of `Caches` (which iOS may purge under storage pressure) into
    /// Application Support; then, with the dongle's own image, into a per-device name. A phone
    /// that installed anywhere between those two moves holds Application
    /// Support/`firmware-latest.bin` — the location the migration knew nothing about, so the
    /// file went invisible and the launch gate lost its offline lifeline with it.
    static func legacyCacheURLs(caches: URL, appSupport: URL) -> [URL] {
        [appSupport.appendingPathComponent(legacyCacheFileName),
         caches.appendingPathComponent(legacyCacheFileName)]
    }

    /// The local cache's full path for `device`, under `dir` (the caller's Application Support
    /// directory — `UpdateClient` owns finding and creating it; this only joins the two). Pure,
    /// and host-tested here rather than only at the `Device.cacheFileName` value layer: the risk
    /// this whole split exists to close — a car image and a dongle image landing under the same
    /// file — lives at the point a device turns into an actual URL, not one layer above it where
    /// `UpdateClient`'s `@MainActor` `ObservableObject` sits outside what `swiftc` host-builds.
    static func cacheURL(for device: Device, in dir: URL) -> URL {
        dir.appendingPathComponent(device.cacheFileName)
    }

    /// What a caller shaped like `AppFlow.performDongleUpdate` should do to obtain the bytes to
    /// flash: fetch a specific asset, reuse what is already on disk, or give up because neither
    /// is available. Pure — the caller still performs the actual download and disk read; this
    /// only decides which of those three shapes applies.
    ///
    /// Every case carries `device` straight through from `flashPlan(for:...)`'s own argument
    /// rather than making the caller re-name it at each downstream use
    /// (`UpdateClient.download(device:)`, `UpdateClient.cachedBinURL(for:)`). That is the whole
    /// point of returning a value instead of a bare URL: the ONE place a caller can name the
    /// wrong device is the `flashPlan(for:...)` call itself, which a host test reaches, rather
    /// than that plus every place its result gets used, which only a bench with the wrong
    /// firmware pushed to it would ever catch.
    enum FlashPlan: Equatable {
        /// Fetch `url` and record it, on success, as `device`'s own cached build/tag.
        case download(device: Device, url: URL, recordBuild: Int?, tag: String)
        /// A previously downloaded, previously validated image for `device` is already on
        /// disk — `UpdateClient.cachedBinURL(for: device)` names it.
        case useCache(device: Device)
        /// No fresh release and nothing usable already cached. Nothing to flash this round.
        case unavailable
    }

    /// - Parameters:
    ///   - device: Whose image this decides for — carried into every non-`unavailable` case of
    ///     the result, unchanged.
    ///   - release: The freshly fetched release for `device` specifically (its own tag and its
    ///     own asset URL — `UpdateClient.latestRelease(for: device)`), or `nil` when GitHub was
    ///     unreachable or unusable.
    ///   - cachedBuild: `UpdateClient.cachedBuild(for: device)` — `device`'s own recorded build,
    ///     never the other device's.
    ///   - hasCachedFile: `UpdateClient.hasCachedFile(for: device)` — `device`'s own cache file.
    static func flashPlan(for device: Device, release: (tag: String, assetURL: URL)?,
                          cachedBuild: Int?, hasCachedFile: Bool) -> FlashPlan {
        let latestBuild = buildNumber(release?.tag)
        if needsDownload(latestBuild: latestBuild, cachedBuild: cachedBuild, hasCachedFile: hasCachedFile) {
            guard let release else { return .unavailable }
            return .download(device: device, url: release.assetURL,
                             recordBuild: buildNumber(release.tag), tag: release.tag)
        }
        return hasCachedFile ? .useCache(device: device) : .unavailable
    }

    /// Normalize a version like "v1.2" / "v1.2-3-gabc" → "1.2" for comparison.
    static func normalize(_ v: String?) -> String {
        guard let v else { return "" }
        var s = v
        if s.hasPrefix("v") { s.removeFirst() }
        if let dash = s.firstIndex(of: "-") { s = String(s[s.startIndex..<dash]) }
        return s
    }

    /// Build number after the first "+" (e.g. "v1.2+246" -> 246); nil if absent/non-numeric.
    static func buildNumber(_ version: String?) -> Int? {
        guard let version, let plus = version.firstIndex(of: "+") else { return nil }
        let digits = version[version.index(after: plus)...].prefix { $0.isNumber }
        return digits.isEmpty ? nil : Int(digits)
    }

    /// Update available iff both versions carry a build number and latest > running.
    /// Falls back to normalized string inequality when a build number is missing.
    static func isUpdateAvailable(running: String?, latest: String?) -> Bool {
        if let r = buildNumber(running), let l = buildNumber(latest) { return l > r }
        return normalize(latest) != normalize(running)
    }

    /// Need to (re)download the .bin: only when there IS a versioned latest release, and the
    /// cached file is missing or its build differs from the latest.
    static func needsDownload(latestBuild: Int?, cachedBuild: Int?, hasCachedFile: Bool) -> Bool {
        guard let latestBuild else { return false }
        return !hasCachedFile || cachedBuild != latestBuild
    }

    /// Forced update required iff the latest release carries a build number AND either the
    /// running firmware predates versioning (no build number) or its build is lower.
    ///
    /// Already answerable for either device despite the `carFw` label (kept because
    /// `UpdateClient.mustUpdate(carFw:latestTag:)` is a fielded call site): this only ever
    /// compares two version strings, and one release tags both images identically, so calling
    /// this with the dongle's own running firmware and the same `latestTag` answers the dongle's
    /// gate — independently of the car's, since neither call carries or mutates any state.
    static func mustUpdate(carFw: String?, latestTag: String?) -> Bool {
        guard let latest = buildNumber(latestTag) else { return false }
        guard let car = buildNumber(carFw) else { return true }
        return latest > car
    }

    /// How long the phone waits for an upload of `bytes` before giving up on it.
    ///
    /// This is a BACKSTOP, not the detector. `CarTransport`'s timeout is a total deadline armed
    /// once when the connection opens — it does not reset on progress — so it cannot tell a slow
    /// transfer from a dead one. What can is the device: both `/ota` implementations abandon a
    /// transfer that goes SILENT for about thirty seconds, and reset that budget on every chunk
    /// that arrives. That rule is the real detector, and it is on the right side of the wire.
    ///
    /// It was a flat 45 s, justified by "the device abandons a stalled upload after ~30 s, so
    /// anything longer is the phone watching a corpse". The premise was a misreading — stalled is
    /// not the same as slow — and the number was sized for a 0.76 MB car image. That image now
    /// carries the radio's too and is 1.83 MB, and it crosses a Wi-Fi hop and a USB relay to get
    /// there. At 45 s flat the phone would kill an upload the car was happily receiving.
    ///
    /// So: allow at least 10 KB/s, with a floor for the small transfers. Generous on purpose —
    /// being wrong in this direction costs a longer wait before a failure the device has already
    /// detected; being wrong the other way fails an update that was working.
    static func uploadTimeout(bytes: Int) -> TimeInterval {
        max(60, Double(bytes) / 10_000)
    }

    /// Decision 6: what may enter the firmware cache. An ESP application image starts with
    /// 0xE9 and the car rejects anything under 4 KB — a GitHub error page satisfies neither,
    /// and caching one used to poison the cache until a strictly newer release existed.
    static func isValidImage(firstByte: UInt8?, size: Int) -> Bool {
        firstByte == 0xE9 && size >= 4096
    }
}
