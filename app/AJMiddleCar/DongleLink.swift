import Foundation

/// The launch sequence's dongle half, as one pure decision: given what the last read of
/// `/status` produced (`DongleReply`), the latest release this phone knows about, the network
/// the car actually expects, and what the user has already said about a standing rollback
/// (`RollbackChoice`), what does the app do next.
///
/// Pure by design — no `async`, no networking, no `@MainActor` — so every branch is
/// host-tested rather than reasoned about against a device that may or may not be on the
/// bench. The impure flow (`AppFlow`) owns the polling, the HTTP calls this implies, and the
/// screen that renders each `DongleStep`; this file owns only which step is next.
///
/// The sequence, in the order the spec states it: presence, then identity, then the dongle's
/// own update, then whether it has been told the RIGHT network, then the join, then — once
/// `.readyForCar` — the car's own existing gate takes over unchanged.
public enum DongleStep: Equatable {
    /// Nothing answered at the dongle's address — not "an error", just the step that tells the
    /// user to plug one in. `DongleReply.silent` is exactly this: the pure module owns
    /// "absent" as a decision, not something the flow special-cases separately.
    case plugIn
    /// Something answered and it was not usable — an HTTP error, a truncated stream, a body
    /// that would not decode (`DongleReply.faulty`). Distinct from `.plugIn` because the one
    /// instruction `.plugIn` gives is the one thing already done: the cable is in and something
    /// on the other end is talking.
    case faulty
    /// Local-network access is denied, so nothing this app sends ever leaves the phone
    /// (`DongleReply.denied`). Not a wait — the user has to change it in Settings — and
    /// certainly not "plug in an adapter".
    case accessDenied
    /// Something is answering at the dongle's address and it is not our dongle: `status.device`
    /// disagrees with `DongleContract.device`. The spec names that field for exactly this —
    /// "how the app tells this apart from any other USB-Ethernet adapter the user might plug
    /// in" — and this is its analogue of the car's own wrong-car screen. `device` is what
    /// answered, so the screen can name it.
    case wrongDongle(device: String)
    /// The bootloader reverted the dongle's last update (`DongleStatus.rollback`). Reported
    /// ahead of the update check that would otherwise re-offer the very image that just failed,
    /// forever — see `next(...)`'s own doc for the ordering. Standing until the user answers
    /// (`RollbackChoice`): the app must give a way past this, and — because the flag is sticky
    /// and the app is the only OTA path — a way back TO an update too, or the only exit left is
    /// a bench reflash.
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
    /// Pointed at the right network and the radio is still working: `joining` (its own bounded
    /// budget is running) or an `unknown` value this build does not recognise. Wait. Never
    /// re-POST from here: a re-POST is a retry request, and nothing has failed yet.
    /// The radio is scanning and has not seen the car's network. Not a failure — the budget is
    /// still running — but a different thing to say than `waiting`, because the likely cause is
    /// a car that is switched off rather than a connection in progress.
    case searchingCar
    case waiting
    /// Pointed at the right network, and the dongle will not get any further on its own:
    /// `net.state == .failed` (the budget its own join policy allows ran out) or `.idle` (its
    /// state machine never left IDLE — see `next(...)`'s branch for how a CONFIGURED dongle
    /// gets there). The credentials are already stored and correct; what is needed is asking
    /// the radio to try again, which is a POST, which is what this step is.
    case retryJoin
    /// Pointed at the right network and `net.state == .connected`: the pipe is up. Hand off to
    /// the car's own existing gate, unchanged — this step exists so the flow knows to stop
    /// asking the dongle anything further, not to replace what happens next.
    case readyForCar
}

/// What the user has already said about a standing rollback — the third input to the rollback
/// branch of `next(...)`, and what keeps that branch from being a one-way exit.
///
/// The dongle's rollback flag is sticky: `status_api.c` reads `ESP_OTA_IMG_ABORTED` from the
/// other partition once at boot, so it clears only when a LATER OTA to that slot succeeds. The
/// app is the only OTA path there is. So a model where acknowledging the rollback permanently
/// suppresses `.updating` leaves exactly one exit — a bench reflash — which is the thing the
/// rollback design exists to prevent.
public enum RollbackChoice: Equatable {
    /// Nothing said yet: report the rollback and stop there.
    case unanswered
    /// "Drive on what is running." The rollback stops being reported, and the update that
    /// produced it is not re-offered: from here there is no way to tell "a fixed release has
    /// since been published" apart from "the same one that just failed".
    case proceed
    /// "Check again" — the affordance `FirmwareView`'s own rolled-back car screen keeps beside
    /// its skip (`fw.retry`). One look at whatever the release feed now says, measured from the
    /// tag that was on offer when it was asked (`nil` when nothing was known then: offline, no
    /// cache). A release NEWER than that is a different image and is offered; the same one is
    /// not re-flashed into the same rollback. The flow consumes this after one decision — one
    /// look per ask, not a standing permission.
    case recheck(from: String?)
}

/// What the last attempt to read `/status` produced.
///
/// "Nothing usable came back" is three different situations with three different things to say,
/// and the flow used to collapse all of them into one `nil` with `try?` — which then rendered as
/// "plug in an adapter" at a user whose adapter is plugged in and answering an HTTP 500. On a
/// branch whose whole justification is that the app half and the dongle half fail in the same
/// place with the same symptom, the reason the dongle gave is the one signal that tells them
/// apart, and it is not something to throw away.
public enum DongleReply {
    /// A `/status` document this build could decode. Whether it describes OUR dongle is
    /// `next(...)`'s first question, not this one's.
    case status(DongleStatus)
    /// Nothing answered: no cable, a refused connection, a deadline that expired with no bytes.
    case silent
    /// Something answered and it was not usable: an HTTP error status, a truncated stream, or a
    /// body that did not decode. Whatever else is true, a device is there and talking.
    case faulty
    /// iOS refused to let the request leave the phone at all: local-network access is denied.
    case denied

    /// Classify what `DongleClient.status()` threw. Pure, and here rather than in the flow so
    /// the rule is host-tested — the flow's job is to catch, log and pass it on.
    public static func of(_ error: Error) -> DongleReply {
        if let e = error as? CarError {
            switch e {
            // Bytes arrived, and they were the far end answering for itself.
            case .http, .truncated: return .faulty
            case .denied: return .denied
            // `.malformed` sits on this side deliberately: `CarError.from` uses it for any
            // `NWError` that is neither an unsatisfied path nor ECONNREFUSED, and
            // `DongleClient`'s request type uses it for a connection that closed without a
            // parseable head. Neither is evidence that a dongle answered.
            case .noDongle, .refused, .timeout, .malformed: return .silent
            }
        }
        // A `DecodingError` is a complete body this build could not read — something answered.
        // Anything else, a cancellation most of all, is evidence of nothing.
        return error is DecodingError ? .faulty : .silent
    }
}

/// The one function `AppFlow` switches on.
public enum DongleLink {
    /// - Parameters:
    ///   - reply: What the last read of `/status` produced — a decoded document, or one of the
    ///     three ways it can fail to be one. See `DongleReply`: the flow classifies, this
    ///     decides, and neither collapses "nothing answered" into "answered badly".
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
    ///   - rollback: What the user has already said about a standing rollback — see
    ///     `RollbackChoice`. `.unanswered` reports it, `.proceed` drives on the firmware that is
    ///     actually running, and `.recheck` is the one path back to an update.
    public static func next(reply: DongleReply, latestTag: String?, expectedSSID: String,
                            rollback: RollbackChoice = .unanswered) -> DongleStep {
        let status: DongleStatus
        switch reply {
        case .status(let s): status = s
        case .silent: return .plugIn
        case .faulty: return .faulty
        case .denied: return .accessDenied
        }

        // Identity first, before a single other field of this document is believed. The spec
        // puts `device` in `/status` for one reason — "how the app tells this apart from any
        // other USB-Ethernet adapter the user might plug in" — and a foreign adapter's `fw`,
        // `rollback` and `net` describe a device this app knows nothing about. Reading them
        // anyway ends in one of two places: flashing our image onto it, or handing it the car's
        // credentials. Both are worse than a screen that says which adapter answered.
        guard status.device == DongleContract.device else {
            return .wrongDongle(device: status.device)
        }

        if status.rollback {
            switch rollback {
            case .unanswered:
                return .rolledBack
            case .proceed:
                // Proceed on the firmware the dongle actually has. Deliberately does NOT fall
                // through to the `mustUpdate` check below — there is no way from here to tell
                // "a fixed release has since been published" apart from "the same one that just
                // failed", and re-offering either looks identical to the loop this exists to
                // break. The user asked to drive on what is running; that is what happens.
                break
            case .recheck(let from):
                // The one path back to the app's own OTA. Newer than what was on offer when
                // they asked, AND newer than what the dongle is running: the first keeps the
                // image that just rolled back from being re-flashed into the same rollback, the
                // second is the ordinary update question. Both, or nothing happens.
                if UpdateRules.isUpdateAvailable(running: from, latest: latestTag),
                   UpdateRules.mustUpdate(carFw: status.fw, latestTag: latestTag) {
                    return .updating
                }
                // Nothing newer to try. Back to the standing report and its two buttons rather
                // than proceeding as though they had asked to skip: the question was "is there
                // a fix yet", and the answer is no.
                return .rolledBack
            }
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
        // Both of these mean "the dongle will not get any further by itself".
        //
        // `failed` is the plain one: the join budget ran out. `idle` is the edge — and it is
        // NOT "never configured", which already returned above on the SSID comparison. It is
        // the case `wifi_sta.c` documents at the very line this branch used to cite for the
        // opposite conclusion: when `esp_wifi_set_config` fails, `wifi_sta_join` logs, returns
        // early and leaves the state machine untouched — "net.state still reflects the previous
        // attempt, not this request... The two legitimately disagree until this is retried (a
        // corrected POST /net, which restarts the whole budget)". And IDLE has exactly one exit,
        // `WIFI_EV_CONFIGURED` (`firmware/s3/main/wifi_state.c`), raised only by
        // `wifi_sta_join`, which only a POST /net (or a boot) calls. So nothing the dongle does
        // on its own leaves this state: waiting here waits forever. Reachable from a stored
        // network the radio refused at boot, and from a POST /net that stored the config and
        // then answered 500.
        case .failed, .idle: return .retryJoin
        case .searching: return .searchingCar
        // `joining` is the radio working through its own bounded budget with the network in
        // sight; `unknown` is a state this build does not know. Neither is a failure a re-POST
        // would fix.
        case .joining, .unknown: return .waiting
        }
    }
}
