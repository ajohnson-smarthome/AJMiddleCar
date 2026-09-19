import Foundation

/// What the user has already said about a standing rollback — the third input to the rollback
/// branch of `VersionRule.step(...)`, and what keeps that branch from being a one-way exit.
///
/// A board's rollback flag is sticky: `status_api.c` reads `ESP_OTA_IMG_ABORTED` from the
/// other partition once at boot, so it clears only when a LATER OTA to that slot succeeds. The
/// app is the only OTA path there is.
///
/// There used to be a `.proceed` here — "drive on what is running" — and it is gone by decision:
/// being on the newest firmware is a precondition for driving, with no exception for the case
/// where updating turns out to be impossible. The consequence was weighed and accepted: an image
/// the bootloader keeps rejecting leaves the adapter behind and the car undriveable until a newer
/// release ships. `.recheck` is the only answer, and a newer release the only way out.
public enum RollbackChoice: Equatable {
    /// Nothing said yet: report the rollback and stop there.
    case unanswered
    /// "Check again" — the one button on a rolled-back screen (S10 for the adapter, S31 for the
    /// car) (`fw.retry`). One look at whatever the release feed now says. `from` is the tag
    /// that was on offer when it was asked (`nil` when nothing was known then: offline, no
    /// cache) — the reference a newer release is measured from only when the phone has no
    /// record of what it last flashed into the board (`VersionRule.step`'s `flashed`). The flow
    /// consumes this after one decision — one look per ask, not a standing permission.
    case recheck(from: String?)
}

/// The decision the launch gate makes for either board from one read of its `/version`.
///
/// Pure by design — no `async`, no networking — so every branch is host-tested. The order is
/// the one `DongleLink` always used: identity, then rollback, then version. `latestTag` is not
/// optional: the gate learns the release first (`fetchRelease`) and asks this only with a tag
/// in hand. `flashed` is the last build this phone flashed into this board (recorded at the
/// flash's `ok` — `UpdateClient.lastFlashedTag`), `nil` when it never has: the reference a
/// rolled-back board's next offer is measured from.
public enum VersionStep: Equatable {
    /// Nothing answered at the board's address.
    case plugIn
    /// Something answered and it was not this document.
    case faulty
    /// iOS refused the request: local-network access is denied.
    case accessDenied
    /// A board that is not the expected one. Named, and nothing else of it is read: a foreign
    /// board must be neither flashed nor handed the car's network.
    case wrongDevice(name: String)
    /// The bootloader reverted the last update, and the one answer (`RollbackChoice.recheck`)
    /// found nothing newer to try.
    case rolledBack
    /// Behind the release — or older than `/version` itself (404). The forced update takes it
    /// across; its `POST /ota` has always existed.
    case updating
    /// Ours, and current. Safe to proceed.
    case ok
}

public enum VersionRule {
    public static func step(reply: VersionReply, expectedDevice: String, latestTag: String,
                            flashed: String?, rollback: RollbackChoice) -> VersionStep {
        let v: DeviceVersion
        switch reply {
        case .version(let d): v = d
        case .absent: return .updating
        case .silent: return .plugIn
        case .faulty: return .faulty
        case .denied: return .accessDenied
        }
        guard v.device == expectedDevice else { return .wrongDevice(name: v.device) }
        if v.rolled_back {
            // The reference a newer release is measured from. The build this phone last
            // flashed into the board, when it has one on record — that is the image that
            // rolled back (or an older one; either way the floor), and it answers the first
            // look, no tap needed. Without a record: the tag on offer when «Повторить» was
            // tapped, and before the tap nothing at all — the rollback stands. Measuring from
            // the tag on hand alone was AJM-132: in any launch after the rollback the ladder
            // learns the newest tag first, so "newer than what was on offer" was "newer than
            // itself", and a rolled-back board never got the fixing release.
            let reference: String?
            switch (flashed, rollback) {
            case (let record?, _): reference = record
            case (nil, .recheck(let from)): reference = from
            case (nil, .unanswered): return .rolledBack
            }
            // Newer than the reference, AND newer than what the board runs: the first keeps
            // the image that rolled back from being re-flashed into the same rollback, the
            // second is the ordinary update question.
            if UpdateRules.isUpdateAvailable(running: reference, latest: latestTag),
               UpdateRules.mustUpdate(carFw: v.fw, latestTag: latestTag) {
                return .updating
            }
            return .rolledBack
        }
        if UpdateRules.mustUpdate(carFw: v.fw, latestTag: latestTag) { return .updating }
        // Protocol is no longer a gate input: one release ships the app and both firmwares
        // together, so "build == the release tag" is the whole of compatibility. `v.proto` is
        // decoded but not read. See spec 2026-09-17-proto-out-of-app.
        return .ok
    }
}
