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

    private let repo = "ajohnson-smarthome/AJPicoCar"

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
    /// Falls back to normalized string inequality when a build number is missing (legacy firmware/releases).
    static func isUpdateAvailable(running: String?, latest: String?) -> Bool {
        if let r = buildNumber(running), let l = buildNumber(latest) { return l > r }
        return normalize(latest) != normalize(running)
    }

    /// Need to (re)download the .bin: only when there IS a versioned latest release, and the
    /// cached file is missing or its build differs from the latest.
    static func needsDownload(latestBuild: Int?, cachedBuild: Int?, hasCachedFile: Bool) -> Bool {
        guard let latestBuild else { return false }   // no versioned release → nothing to fetch
        return !hasCachedFile || cachedBuild != latestBuild
    }

    /// Forced update required iff the latest release carries a build number AND either the running
    /// firmware predates versioning (no build number) or its build is lower.
    static func mustUpdate(carFw: String?, latestTag: String?) -> Bool {
        guard let latest = buildNumber(latestTag) else { return false }  // no versioned release → gate inert
        guard let car = buildNumber(carFw) else { return true }          // pre-versioning firmware → must update
        return latest > car
    }

    // MARK: - Internet reachability + firmware cache

    /// Lightweight reachability probe to GitHub (distinguishes "no internet" from "API failed").
    static func internetReachable() async -> Bool {
        guard let url = URL(string: "https://api.github.com") else { return false }
        var req = URLRequest(url: url); req.httpMethod = "HEAD"; req.timeoutInterval = 4
        if let (_, resp) = try? await URLSession.shared.data(for: req) {
            return (resp as? HTTPURLResponse) != nil
        }
        return false
    }

    private static let kBuild = "cachedLatestBuild"
    private static let kTag   = "cachedLatestTag"

    static var cachedBinURL: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("firmware-latest.bin")
    }
    static var cachedBuild: Int? {
        let v = UserDefaults.standard.integer(forKey: kBuild); return v == 0 ? nil : v
    }
    static var cachedTag: String? { UserDefaults.standard.string(forKey: kTag) }
    static var hasCachedFile: Bool { FileManager.default.fileExists(atPath: cachedBinURL.path) }
    static func recordCache(build: Int, tag: String) {
        UserDefaults.standard.set(build, forKey: kBuild)
        UserDefaults.standard.set(tag, forKey: kTag)
    }

    func latestRelease() async -> Release? {
        guard let url = URL(string: "https://api.github.com/repos/\(repo)/releases/latest") else { return nil }
        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let tag = j["tag_name"] as? String,
                  let assets = j["assets"] as? [[String: Any]] else { return nil }
            let bin = assets.first { ($0["name"] as? String)?.hasSuffix(".bin") == true }
            guard let s = bin?["browser_download_url"] as? String, let u = URL(string: s) else { return nil }
            return Release(tag: tag, assetURL: u)
        } catch { return nil }
    }

    func download(_ url: URL) async -> URL? {
        downloadProgress = 0
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        do {
            let (tmp, _) = try await session.download(from: url)
            let dest = UpdateClient.cachedBinURL
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.moveItem(at: tmp, to: dest)
            return dest
        } catch { return nil }
    }

    func upload(_ binURL: URL) async -> Bool {
        guard let url = URL(string: CarHost.httpBase + "/ota") else { return false }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 120
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        do {
            let (_, resp) = try await session.upload(for: req, fromFile: binURL)
            return (resp as? HTTPURLResponse)?.statusCode == 200
        } catch { return false }
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
