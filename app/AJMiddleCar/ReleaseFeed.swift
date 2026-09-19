import Foundation

/// The release feed's own pace, and its refusals (AJM-136).
///
/// The feed is GitHub Releases, read without a token, and GitHub allows an unauthenticated
/// address 60 requests an hour. The launch ladder's holds — «нет интернета», «нет выпуска для
/// платы» — used to ask the feed again on every poll of the boards, every ~1.8 s: about 2000
/// requests an hour, the quota gone within two minutes, and the 403 that followed was read as a
/// body without a tag → «нет интернета» — on a phone whose internet was fine, with the address
/// (and, behind NAT, the whole bench) locked out for the rest of the hour. So the feed has a pace
/// of its own, slower than the boards', and a refusal is read as what it is.
///
/// Pure: `AppFlow` keeps the miss count and the clock; `UpdateClient` hands the status and the
/// two headers here rather than reading them itself.
public enum ReleaseFeed {
    /// The hold's first wait before the feed is asked again, and the ceiling it grows to. The
    /// boards keep the ladder's own poll interval; only the feed is paced. At the ceiling a hold
    /// spends the hour's quota exactly, no more — and a refusal, below, waits the rest out.
    public static let holdFloor: TimeInterval = 20
    public static let holdCeiling: TimeInterval = 60

    /// How long to wait before asking again after `misses` reads in a row without a tag:
    /// 20 s, 40 s, 60 s, 60 s… — the floor, then a floor more per miss, up to the ceiling. Below
    /// one miss there is nothing to wait for.
    public static func holdInterval(afterMisses misses: Int) -> TimeInterval {
        guard misses > 0 else { return 0 }
        return min(holdCeiling, holdFloor * Double(misses))
    }

    /// What one answer of the feed is, by its status line.
    public enum Answer: Equatable {
        /// 200: a document to read.
        case document
        /// 403 or 429: the feed is refusing this address — GitHub's primary limit answers 403
        /// with `x-ratelimit-remaining: 0`, its secondary limits 429 — and named, or was given,
        /// this long a wait. Not «нет интернета»: the feed answered.
        case refused(retryAfter: TimeInterval)
        /// Anything else: not a document this build can use.
        case unusable
    }

    /// The wait when a refusal names none.
    public static let refusedDefaultWait: TimeInterval = 60
    /// The most a refusal may make the launch wait, whatever its headers say: an hour is the
    /// primary limit's whole window, and a reset further off than that is a clock, not a limit.
    public static let refusedMaxWait: TimeInterval = 3600

    /// Classify by status and the two headers GitHub uses: `retry-after`, in seconds, on the
    /// secondary limits; `x-ratelimit-reset`, an epoch second, on every answer — the end of the
    /// primary limit's window when that is what refused. `now` is the same epoch clock.
    public static func classify(status: Int, retryAfter: String?, rateLimitReset: String?,
                                now: TimeInterval) -> Answer {
        switch status {
        case 200: return .document
        case 403, 429:
            return .refused(retryAfter: refusalWait(retryAfter: retryAfter, rateLimitReset: rateLimitReset, now: now))
        default: return .unusable
        }
    }

    /// The wait a refusal names — `retry-after` first (GitHub's own order of precedence), else
    /// the time left to `x-ratelimit-reset`, else the default — clamped to [`holdFloor`,
    /// `refusedMaxWait`]: a reset already past, or a zero, is still a pause and not a hot loop,
    /// and a reset days away cannot park the launch for a day.
    static func refusalWait(retryAfter: String?, rateLimitReset: String?, now: TimeInterval) -> TimeInterval {
        let named: TimeInterval?
        if let s = retryAfter.flatMap(seconds) {
            named = s
        } else if let reset = rateLimitReset.flatMap(seconds) {
            named = reset - now
        } else {
            named = nil
        }
        return min(refusedMaxWait, max(holdFloor, named ?? refusedDefaultWait))
    }

    /// A header's integer seconds, or nil for anything else — an HTTP-date `retry-after` is
    /// legal but not what GitHub sends, and the default covers it.
    private static func seconds(_ header: String) -> TimeInterval? {
        let trimmed = header.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed.allSatisfy(\.isNumber) else { return nil }
        return TimeInterval(trimmed)
    }
}
