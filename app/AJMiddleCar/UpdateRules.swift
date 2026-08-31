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

    /// The local cache's full path for `device`, under `dir` (the caller's Application Support
    /// directory — `UpdateClient` owns finding and creating it; this only joins the two). Pure,
    /// and host-tested here rather than only at the `Device.cacheFileName` value layer: the risk
    /// this whole split exists to close — a car image and a dongle image landing under the same
    /// file — lives at the point a device turns into an actual URL, not one layer above it where
    /// `UpdateClient`'s `@MainActor` `ObservableObject` sits outside what `swiftc` host-builds.
    static func cacheURL(for device: Device, in dir: URL) -> URL {
        dir.appendingPathComponent(device.cacheFileName)
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

    /// Decision 6: what may enter the firmware cache. An ESP application image starts with
    /// 0xE9 and the car rejects anything under 4 KB — a GitHub error page satisfies neither,
    /// and caching one used to poison the cache until a strictly newer release existed.
    static func isValidImage(firstByte: UInt8?, size: Int) -> Bool {
        firstByte == 0xE9 && size >= 4096
    }
}
