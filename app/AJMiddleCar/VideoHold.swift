import Foundation

/// When the video socket may (re)open after the car accepted a new bitrate. Pure,
/// host-tested (`app/tests/videohold`); `VideoLink` keeps one and asks it before every open.
///
/// The car reads `bitrate_kbps` once, at stream start (`car/video-stream`), and a `view`
/// inside `video.subscribe_timeout_ms` of the last one is a refresh of the running stream,
/// not a new one. The settings sheet takes the subscription down while it is open, so a
/// slider moved there and the sheet closed within three seconds met the car's subscription
/// still alive: same stream, old bitrate, and «Действует со следующего запуска картинки»
/// named a moment the driver could not see (AJM-122). The rule: a `video` write the car
/// accepted with another bitrate, the switch staying on, holds the next open until the
/// car's subscription has certainly lapsed — the timeout after the socket last spoke, plus
/// a margin for the car's subscription clock and the relay.
struct VideoReopenHold: Equatable {
    /// The car's subscription timeout, from the contract.
    static let timeout: TimeInterval = Double(CarContract.videoSubscribeTimeoutMs) / 1000
    /// On top of the timeout: the car judges expiry once per 100 ms tick of its control task
    /// (`video_link.c`, `CTL_TICK_MS`) and reads an arriving `view` *before* judging, so a
    /// view at the timeout exactly would still refresh the old stream; the relay adds tens of
    /// milliseconds on top. Half a second covers both with room and is not a black screen.
    static let margin: TimeInterval = 0.5

    private(set) var until: TimeInterval?

    /// Whether this accepted write changes a running stream in a way only a restart applies:
    /// the bitrate moved while the switch stays on. Switching off ends the stream on the car
    /// at once; switching on starts one that reads the new bitrate — neither needs a hold.
    static func restartsStream(from old: Video, to new: Video) -> Bool {
        old.enabled && new.enabled && old.bitrate_kbps != new.bitrate_kbps
    }

    /// Hold the next open until the subscription opened by the last `view` has lapsed. A
    /// later arm moves the hold later, never earlier.
    mutating func arm(lastViewAt: TimeInterval) {
        let t = lastViewAt + Self.timeout + Self.margin
        until = max(until ?? t, t)
    }

    /// Seconds to wait before opening; `nil` — open now.
    func delay(now: TimeInterval) -> TimeInterval? {
        guard let until, until > now else { return nil }
        return until - now
    }
}
