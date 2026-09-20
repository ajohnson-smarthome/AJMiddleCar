/// Whether to ask the car for video, and whether the driver's input is taken. Pure,
/// host-tested (`app/tests/drivemode`): the car's `video` config and whether something covers
/// the screen in, the subscription gate and the input gate out — the one place both are
/// decided, so the screen cannot watch from under a sheet, or drive from under one. The
/// layout is not decided here: the drive screen has one (`openspec/specs/app/drive-hud`), and
/// the car's switch only says whether its window is live.
///
/// `nil` — the config has not been read yet — is no views: the car ignores them when the
/// switch is off, and the window would light up and go dark again if it is on. The config is
/// prefetched when the car is met, so this is a moment at most (`app/drive-session`).
///
/// `covered` is anything drawn over the drive screen: the settings sheet, the mandatory
/// calibration wizard, the «Поиск…» veil of a telemetry pause inside a live session. Under
/// any of them the car stands — the intent goes to zero when the cover arrives and the
/// gamepad and the stick are ignored until it is gone (AJM-101). Before, `covered` only
/// decided the subscription, and a deflected gamepad drove an uncalibrated car between the
/// wizard's spin pulses.
struct DriveScreenState: Equatable {
    /// Whether `view` datagrams go to the car: the switch confirmed on, and nothing over the
    /// screen. The window itself stays live under a sheet — only the subscription lapses.
    let watching: Bool
    /// Whether the joystick and the gamepad move the intent. False exactly while covered.
    let inputAllowed: Bool
}

/// What a tap on the video button does. The domain unread, the button used to be dead, and
/// the drive screen has no other path to a retry: one failed `GET /config` at session start
/// left the driver with an empty window and without a word why (AJM-120).
enum VideoTap: Equatable {
    /// The domain is not read: re-read it — a GET, never a write over a value nobody saw.
    case reread
    /// The domain is read: post it whole, the switch flipped, the bitrate as the car has it.
    case toggle(Video)
}

enum DriveModeRule {
    static func state(config: Video?, covered: Bool) -> DriveScreenState {
        let on = config?.enabled ?? false
        return DriveScreenState(watching: on && !covered, inputAllowed: !covered)
    }

    static func videoTap(config: Video?) -> VideoTap {
        guard let config else { return .reread }
        return .toggle(Video(bitrate_kbps: config.bitrate_kbps, enabled: !config.enabled))
    }
}
