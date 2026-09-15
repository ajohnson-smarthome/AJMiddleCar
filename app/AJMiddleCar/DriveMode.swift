/// Which drive screen to show, and whether to ask the car for video. Pure, host-tested
/// (`app/tests/drivemode`): the car's `video` config and whether a sheet covers the screen
/// in, the layout and the subscription gate out — the one place both are decided, so the
/// screen cannot show the HUD while not watching, or watch while showing the old layout.
///
/// `nil` — the config has not been read yet — is the layout from before video, and no
/// views: the car ignores them when the switch is off, and the HUD would flash on and then
/// switch away if it is. The config is prefetched when the car is met, so this is a moment
/// at most (docs/superpowers/specs/2026-09-15-video-switch-design.md, §3).
enum DriveMode: Equatable {
    /// The picture as the screen, instruments on its edges (drive-hud-design).
    case hud
    /// The screen from before video: diagram in the middle, sticks and tricks below.
    case classic
}

struct DriveScreenState: Equatable {
    let mode: DriveMode
    let watching: Bool
}

enum DriveModeRule {
    static func state(config: Video?, covered: Bool) -> DriveScreenState {
        guard let config, config.enabled else { return DriveScreenState(mode: .classic, watching: false) }
        return DriveScreenState(mode: .hud, watching: !covered)
    }
}
