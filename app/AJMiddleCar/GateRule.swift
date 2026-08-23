import Foundation

/// The launch gate's offline rule, pure so it is host-tested.
///
/// GitHub being unreachable must not strand a phone standing next to a healthy car: when a
/// cached firmware image exists (the file and its recorded build), the gate hands over and
/// the forced-update comparison simply has no `latestTag` to work with — the same inert
/// state a release without a build number produces.
enum GateRule {
    static func canProceedOffline(hasCachedFile: Bool, cachedBuild: Int?) -> Bool {
        hasCachedFile && cachedBuild != nil
    }

    /// Decision 4a: the tag the forced gate compares against when GitHub was unreachable —
    /// the last release this phone downloaded, but only while the cached image that tag
    /// describes still exists. Without this an offline launch left `latestTag` nil for the
    /// whole session and the forced gate silently never fired.
    static func offlineLatestTag(cachedTag: String?, hasCachedFile: Bool,
                                 cachedBuild: Int?) -> String? {
        guard canProceedOffline(hasCachedFile: hasCachedFile, cachedBuild: cachedBuild) else {
            return nil
        }
        return cachedTag
    }
}
