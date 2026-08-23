import Foundation

/// The update chain's pure decisions, extracted from `UpdateClient` so they are host-tested.
/// `UpdateClient` keeps the sockets, the cache files and the sessions; this answers "which
/// version wins" and "what may enter the firmware cache".
enum UpdateRules {
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
