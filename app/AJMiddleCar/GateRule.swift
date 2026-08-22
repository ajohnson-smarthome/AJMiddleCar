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
}
