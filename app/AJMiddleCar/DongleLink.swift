import Foundation

/// The launch sequence's dongle half, as one pure decision: given what the dongle last said
/// about itself and the latest release this phone knows about, what does the app do next.
///
/// Pure by design — no `async`, no networking, no `@MainActor` — so every branch is
/// host-tested rather than reasoned about against a device that may or may not be on the
/// bench. The impure flow (`AppFlow`) owns the polling, the HTTP calls this implies, and the
/// screen that renders each `DongleStep`; this file owns only which step is next.
///
/// The sequence, in the order the spec states it: presence, then the dongle's own update,
/// then whether it has been told a network, then the join, then — once `.readyForCar` — the
/// car's own existing gate takes over unchanged.
public enum DongleStep: Equatable {
    /// No dongle answered `/status` at all — not "an error", just the step that tells the
    /// user to plug one in. `DongleLink.next(status: nil, ...)` is exactly this: the pure
    /// module owns "absent" as a decision, not something the flow special-cases separately.
    case plugIn
    /// The bootloader reverted the dongle's last update (`DongleStatus.rollback`). Reported
    /// unconditionally, ahead of every other check: in the one way this actually arises — a
    /// release still ahead of the version the dongle reverted TO — the update check below
    /// would otherwise recompute `.updating` and offer the very image that just failed,
    /// forever. Saying so, once, is what breaks that loop; the dongle stays on the working
    /// image it has rather than being pushed to retry a build the bootloader already rejected.
    case rolledBack
    /// The dongle's own firmware is behind `latestTag`. Comes before every net/join question
    /// on purpose — the spec's own words: "The dongle updates before the car... Settle the
    /// pipe before pushing the long transfer down it." A dongle mid-relay-bug is not something
    /// to hand credentials to first.
    case updating
    /// Current, but never told a network (`net.ssid` empty — see the doc below for why that,
    /// not a separate `configured` flag, is the signal). Send the car's own credentials.
    case sendCredentials
    /// Configured and the radio is between attempts (`idle`, `joining`, or an `unknown` value
    /// this build does not recognise) — wait. Never re-POST from here: a re-POST is a retry
    /// request, and nothing failed yet.
    case waiting
    /// `net.state == .failed`: the budget the dongle's own join policy allows ran out. The
    /// credentials are already stored and correct — what failed was the radio reaching the
    /// car — so this is the retry step, not the configure step.
    case retryJoin
    /// `net.state == .connected`: the pipe is up. Hand off to the car's own existing gate,
    /// unchanged — this step exists so the flow knows to stop asking the dongle anything
    /// further, not to replace what happens next.
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
    public static func next(status: DongleStatus?, latestTag: String?) -> DongleStep {
        guard let status else { return .plugIn }

        // Unconditional, ahead of the update check that would otherwise re-offer the very
        // image the bootloader just rejected — see `.rolledBack`'s doc for why checking it
        // here, rather than only when `mustUpdate` is also true, changes nothing reachable
        // (a rollback's running firmware is definitionally still behind `latestTag`) while
        // staying the simpler, more conservative read of "the app must say so".
        if status.rollback { return .rolledBack }

        if UpdateRules.mustUpdate(carFw: status.fw, latestTag: latestTag) { return .updating }

        // `configured` never arrives as its own field here — `/status` has no such key (only
        // `GET /net` does). `net.ssid` is the same signal by construction: the firmware fills
        // it from `net_api_current(&cfg) ? cfg.ssid : ""`, the identical `s_configured` flag
        // `GET /net`'s `configured` reports (`firmware/s3/main/status_api.c`,
        // `firmware/s3/main/net_api.c`). An empty SSID and "not configured" cannot disagree,
        // so reading emptiness here is not an approximation of the real signal — it IS the
        // real signal, without a second round trip to fetch it.
        guard !status.net.ssid.isEmpty else { return .sendCredentials }

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
