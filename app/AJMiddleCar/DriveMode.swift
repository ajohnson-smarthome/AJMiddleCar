/// Which drive screen to show, whether to ask the car for video, and whether the driver's
/// input is taken. Pure, host-tested (`app/tests/drivemode`): the car's `video` config and
/// whether something covers the screen in, the layout, the subscription gate and the input
/// gate out — the one place all three are decided, so the screen cannot show the HUD while
/// not watching, watch while showing the old layout, or drive from under a sheet.
///
/// `nil` — the config has not been read yet — is the layout from before video, and no
/// views: the car ignores them when the switch is off, and the HUD would flash on and then
/// switch away if it is. The config is prefetched when the car is met, so this is a moment
/// at most (docs/superpowers/specs/2026-09-15-video-switch-design.md, §3).
///
/// `covered` is anything drawn over the drive screen: the settings sheet, the mandatory
/// calibration wizard, the «Поиск…» veil of a telemetry pause inside a live session. Under
/// any of them the car stands — the intent goes to zero when the cover arrives and the
/// gamepad and the stick are ignored until it is gone (AJM-101). Before, `covered` only
/// decided the subscription, and a deflected gamepad drove an uncalibrated car between the
/// wizard's spin pulses.
enum DriveMode: Equatable {
    /// The picture as the screen, instruments on its edges (drive-hud-design).
    case hud
    /// The screen from before video: diagram in the middle, sticks and tricks below.
    case classic
}

struct DriveScreenState: Equatable {
    let mode: DriveMode
    let watching: Bool
    /// Whether the joystick and the gamepad move the intent. False exactly while covered.
    let inputAllowed: Bool
}

/// What a tap on the video button does. The domain unread, the button used to be dead, and
/// the classic layout has no other path to a retry: one failed `GET /config` at session
/// start left the driver without a picture and without a word why (AJM-120).
enum VideoTap: Equatable {
    /// The domain is not read: re-read it — a GET, never a write over a value nobody saw.
    case reread
    /// The domain is read: post it whole, the switch flipped, the bitrate as the car has it.
    case toggle(Video)
}

enum DriveModeRule {
    static func state(config: Video?, covered: Bool) -> DriveScreenState {
        guard let config, config.enabled else {
            return DriveScreenState(mode: .classic, watching: false, inputAllowed: !covered)
        }
        return DriveScreenState(mode: .hud, watching: !covered, inputAllowed: !covered)
    }

    static func videoTap(config: Video?) -> VideoTap {
        guard let config else { return .reread }
        return .toggle(Video(bitrate_kbps: config.bitrate_kbps, enabled: !config.enabled))
    }
}
