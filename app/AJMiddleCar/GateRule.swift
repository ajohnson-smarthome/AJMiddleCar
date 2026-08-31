import Foundation

/// Whether a launch may hand the car over to the driver.
///
/// The rule is deliberately absolute: **being on the newest firmware is a precondition for
/// driving, and so is knowing what the newest firmware is.** Both halves matter, and the second
/// is the one that used to leak. The gate compared the device's build against `latestTag`, and
/// when `latestTag` was nil — GitHub unreachable, or a release with no build number in it — the
/// comparison quietly answered "nothing to update" and the launch sailed through. "I could not
/// check" and "it is current" were the same answer.
///
/// The cost is real and was accepted knowingly: **no internet means no driving.** A phone with no
/// signal cannot verify anything, so it does not hand over, and a cached image no longer buys a
/// pass — the cache says what this phone downloaded once, not what the newest release is now.
///
/// So is the other cost: an image the bootloader keeps rejecting leaves the device behind the
/// newest release with no way to catch up, and this rule then refuses to drive. There is no skip.
/// That is the point of it, and the way out is a newer release, not a button.
///
/// Pure, so both halves are arithmetic anybody can check rather than behaviour anybody has to
/// reproduce on hardware.
enum GateRule {
    /// May a device on `deviceBuild` be driven, given the newest release is `latestBuild`?
    ///
    /// `nil` on either side is a refusal, and for the same reason in both cases: an unknown is
    /// not a pass. An unknown `latestBuild` is "could not check"; an unknown `deviceBuild` is a
    /// device whose own version string this build cannot read, which is no more trustworthy.
    static func mayDrive(deviceBuild: Int?, latestBuild: Int?) -> Bool {
        guard let latestBuild, let deviceBuild else { return false }
        return deviceBuild >= latestBuild
    }

    /// Whether the newest release could be established at all. Separate from `mayDrive` because
    /// the two failures need different screens: "connect to the internet" is actionable, "update
    /// your firmware" is a different instruction entirely, and collapsing them was what let the
    /// first masquerade as success.
    static func canVerify(latestBuild: Int?) -> Bool { latestBuild != nil }
}
