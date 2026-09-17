import Foundation

/// The launch sequence's network half for the adapter, as one pure decision: given the
/// adapter's `/status` — read only once `VersionRule` said `.ok`, so the document is one this
/// build's decoder knows — and the network the car actually expects, what does the app do
/// next. Identity, rollback and version are `VersionRule`'s, decided before this is asked.
public enum DongleStep: Equatable {
    /// Not told the car's network yet — or told some other one (`wifi.ssid` is compared against
    /// the car's own name, not just checked for emptiness): send the credentials.
    case sendCredentials
    /// The radio is scanning and has not seen the car's network: usually a car switched off.
    case searchingCar
    /// `joining`, or a state this build does not know: the radio is working, wait.
    case waiting
    /// `failed` or `idle` with the network in place: the adapter will not get any further on
    /// its own — ask it to try again (bounded by the flow's budget).
    case retryJoin
    /// Joined the car's network: the car may be asked for its version now.
    case readyForCar
}

public enum DongleLink {
    public static func next(status: DongleStatus, expectedSSID: String) -> DongleStep {
        // Compared, not just checked for emptiness: `wifi.ssid` is the same signal `wifi.configured`
        // reports (both come from the firmware's single `s_configured`/`s_cfg` pair —
        // `firmware/dongle/main/status_api.c`, `firmware/dongle/main/net_api.c`), so an empty value
        // still means "never configured". But a NON-empty value that disagrees with
        // `expectedSSID` means "configured for something else" — stale bench credentials, a
        // different car — and that is exactly as unready as empty, not a state to hand off from.
        guard status.wifi.ssid == expectedSSID else { return .sendCredentials }

        switch status.wifi.state {
        case .connected: return .readyForCar
        // Both of these mean "the adapter will not get any further by itself".
        //
        // `failed` is the plain one: the join budget ran out. `idle` is the edge — and it is
        // NOT "never configured", which already returned above on the SSID comparison. With
        // the SSID in place, `idle` means wifi_sta_join could not record the request: its
        // state lock was busy at the one moment it needed it (`wifi_sta.c`: "state lock busy —
        // join requested without recording it"), so the radio was told to connect while the
        // machine still says nothing has been asked. Rare, and nothing the dongle does on its
        // own leaves it — IDLE's one exit is a POST /wifi. So waiting here waits forever, and a
        // re-POST is the fix. (The dongle keeps its network in RAM only, so every boot starts
        // IDLE with an EMPTY ssid — that case is the guard above, not this branch.)
        case .failed, .idle: return .retryJoin
        case .searching: return .searchingCar
        // `joining` is the radio working through its own bounded budget with the network in
        // sight; `unknown` is a state this build does not know. Neither is a failure a re-POST
        // would fix.
        case .joining, .unknown: return .waiting
        }
    }
}
