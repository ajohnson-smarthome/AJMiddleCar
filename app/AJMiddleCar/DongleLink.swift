import Foundation

/// The launch sequence's dongle half, as one pure decision: given what the dongle last said
/// about itself, the latest release this phone knows about, the network the car actually
/// expects, and whether a standing rollback has already been acknowledged, what does the app do
/// next.
///
/// Pure by design — no `async`, no networking, no `@MainActor` — so every branch is
/// host-tested rather than reasoned about against a device that may or may not be on the
/// bench. The impure flow (`AppFlow`) owns the polling, the HTTP calls this implies, and the
/// screen that renders each `DongleStep`; this file owns only which step is next.
///
/// The sequence, in the order the spec states it: presence, then the dongle's own update,
/// then whether it has been told the RIGHT network, then the join, then — once `.readyForCar` —
/// the car's own existing gate takes over unchanged.
public enum DongleStep: Equatable {
    /// No dongle answered `/status` at all — not "an error", just the step that tells the
    /// user to plug one in. `DongleLink.next(status: nil, ...)` is exactly this: the pure
    /// module owns "absent" as a decision, not something the flow special-cases separately.
    case plugIn
    /// The bootloader reverted the dongle's last update (`DongleStatus.rollback`). Reported
    /// ahead of the update check that would otherwise re-offer the very image that just failed,
    /// forever — see `next(...)`'s own doc for the ordering. Standing until `rollbackAcknowledged`
    /// is true: the app must give the user a way past this (the car's own forced-update gate
    /// keeps a skip for the identical reason), not just a message with nowhere to go.
    case rolledBack
    /// The dongle's own firmware is behind `latestTag`. Comes before every net/join question
    /// on purpose — the spec's own words: "The dongle updates before the car... Settle the
    /// pipe before pushing the long transfer down it." A dongle mid-relay-bug is not something
    /// to hand credentials to first.
    case updating
    /// Not pointed at the car's own network — either never told one (`net.ssid` empty) or
    /// pointed at some other one (a stale bench SSID, a previous car). Send the car's own
    /// credentials either way; see `next(...)`'s doc for why comparing against the expected
    /// SSID, not just checking emptiness, is what this case now means.
    case sendCredentials
    /// Pointed at the right network and the radio is between attempts (`idle`, `joining`, or an
    /// `unknown` value this build does not recognise) — wait. Never re-POST from here: a
    /// re-POST is a retry request, and nothing failed yet.
    case waiting
    /// `net.state == .failed`, pointed at the right network: the budget the dongle's own join
    /// policy allows ran out. The credentials are already stored and correct — what failed was
    /// the radio reaching the car — so this is the retry step, not the configure step.
    case retryJoin
    /// Pointed at the right network and `net.state == .connected`: the pipe is up. Hand off to
    /// the car's own existing gate, unchanged — this step exists so the flow knows to stop
    /// asking the dongle anything further, not to replace what happens next.
    case readyForCar
}

/// The one function `AppFlow` switches on.
public enum DongleLink {
    /// - Parameters:
    ///   - status: The dongle's last `/status`, or `nil` when nothing answered — a timeout, a
    ///     refusal, or simply no cable in yet all read the same to this function, because the
    ///     flow that calls it collapses every way of "the dongle did not answer" into one nil
    ///     before asking.
    ///   - latestTag: The latest release tag this phone knows about (possibly from an offline
    ///     cache — see `GateRule`), fed straight into `UpdateRules.mustUpdate`, which already
    ///     answers either device from the same comparison since one release tags both images
    ///     identically.
    ///   - expectedSSID: The car's own network name (`CarContract.ssid`), the one value this
    ///     function compares `status.net.ssid` against. A dongle can read as "configured" while
    ///     pointed at the wrong network entirely — leftover bench credentials, a different car
    ///     — and a comparison against `.isEmpty` alone cannot tell that apart from "pointed at
    ///     ours". Comparing against the expected value can, and is what lets a mis-pointed
    ///     dongle be re-pointed automatically instead of only ever failing to configure once.
    ///   - rollbackAcknowledged: Whether the flow has already shown `.rolledBack` and the user
    ///     chose to proceed anyway (the dongle's equivalent of the car's forced-update skip).
    ///     Once true, a standing rollback stops being reported and stops blocking the update
    ///     check's `false` path — see the doc below for exactly what that unblocks and why it
    ///     does not include quietly retrying the update that just failed.
    public static func next(status: DongleStatus?, latestTag: String?, expectedSSID: String,
                            rollbackAcknowledged: Bool = false) -> DongleStep {
        guard let status else { return .plugIn }

        if status.rollback {
            if !rollbackAcknowledged { return .rolledBack }
            // Acknowledged: proceed on the firmware the dongle actually has. Deliberately does
            // NOT fall through to the `mustUpdate` check below — there is no way from here to
            // tell "a fixed release has since been published" apart from "the same one that
            // just failed", and re-offering either looks identical to the loop this field
            // exists to break. The user asked to drive on what is running; that is what happens.
        } else if UpdateRules.mustUpdate(carFw: status.fw, latestTag: latestTag) {
            return .updating
        }

        // Compared, not just checked for emptiness: `net.ssid` is the same signal `GET /net`'s
        // `configured` reports (both come from the firmware's single `s_configured`/`s_cfg` pair
        // — `firmware/s3/main/status_api.c`, `firmware/s3/main/net_api.c`), so an empty value
        // still means "never configured". But a NON-empty value that disagrees with
        // `expectedSSID` means "configured for something else" — stale bench credentials, a
        // different car — and that is exactly as unready as empty, not a state to hand off from.
        guard status.net.ssid == expectedSSID else { return .sendCredentials }

        switch status.net.state {
        case .connected: return .readyForCar
        case .failed: return .retryJoin
        // `idle` here is not "never configured" — that case already returned above. It is the
        // one documented edge where a configured dongle can still read idle: `wifi_sta_join`
        // logs and returns early when `esp_wifi_set_config` itself fails, leaving the state
        // machine "reflecting the previous attempt" rather than stepping to `.joining`
        // (`firmware/s3/main/wifi_sta.c`). Treating it the same as `.joining` — wait, do not
        // re-POST — is correct either way: nothing failed that a re-POST would fix.
        case .idle, .joining, .unknown: return .waiting
        }
    }
}
