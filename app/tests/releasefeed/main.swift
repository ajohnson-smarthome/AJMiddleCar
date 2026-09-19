// Host test for ReleaseFeed — the release feed's own pace while the launch ladder holds on
// «нет выпуска» / «нет интернета», and what a 403/429 from it means. Pure over counts, HTTP
// status and two headers. Run with swiftc.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

// The pace: the boards are polled every 1.5 s, the feed is not. A hold re-asks it after 20 s,
// then 40, then 60, and stays at 60 — GitHub's unauthenticated limit is 60 an hour per address,
// and a hold that asked on every poll (~2000 an hour) locked the address within two minutes.
check(ReleaseFeed.holdInterval(afterMisses: 0) == 0, "no miss yet: ask now")
check(ReleaseFeed.holdInterval(afterMisses: 1) == 20, "first miss: 20 s")
check(ReleaseFeed.holdInterval(afterMisses: 2) == 40, "second miss: 40 s")
check(ReleaseFeed.holdInterval(afterMisses: 3) == 60, "third miss: 60 s")
check(ReleaseFeed.holdInterval(afterMisses: 100) == 60, "the ceiling holds: 60 s")
check(ReleaseFeed.holdFloor == 20 && ReleaseFeed.holdCeiling == 60, "floor 20, ceiling 60")

// Classification: 200 is a document to read; 403 and 429 are the feed refusing — a
// rate limit, not «нет интернета» — with a wait it names; anything else is unusable.
let now: TimeInterval = 1_800_000_000
check(ReleaseFeed.classify(status: 200, retryAfter: nil, rateLimitReset: nil, now: now) == .document, "200: document")
check(ReleaseFeed.classify(status: 200, retryAfter: "30", rateLimitReset: "\(Int(now) + 100)", now: now) == .document,
      "200 with rate-limit headers (GitHub sends them on every answer): still a document")
check(ReleaseFeed.classify(status: 404, retryAfter: nil, rateLimitReset: nil, now: now) == .unusable, "404: unusable")
check(ReleaseFeed.classify(status: 500, retryAfter: nil, rateLimitReset: nil, now: now) == .unusable, "500: unusable")

// The wait a refusal names: `retry-after` (seconds — secondary limits) first, else the time to
// `x-ratelimit-reset` (an epoch second — the primary limit's hour), else a default.
check(ReleaseFeed.classify(status: 429, retryAfter: "60", rateLimitReset: nil, now: now) == .refused(retryAfter: 60),
      "429 retry-after 60: refused, wait 60")
check(ReleaseFeed.classify(status: 403, retryAfter: nil, rateLimitReset: "\(Int(now) + 1800)", now: now) == .refused(retryAfter: 1800),
      "403 with reset in 30 min: refused, wait 30 min")
check(ReleaseFeed.classify(status: 403, retryAfter: "45", rateLimitReset: "\(Int(now) + 1800)", now: now) == .refused(retryAfter: 45),
      "both headers: retry-after wins")
check(ReleaseFeed.classify(status: 403, retryAfter: nil, rateLimitReset: nil, now: now) == .refused(retryAfter: ReleaseFeed.refusedDefaultWait),
      "403 naming nothing: the default wait")
check(ReleaseFeed.refusedDefaultWait >= ReleaseFeed.holdFloor, "the default wait is no shorter than the hold's floor")
// Bounds: a reset already in the past, or garbage, is the floor; a reset days away is an hour —
// a bad clock must not park the launch for a day.
check(ReleaseFeed.classify(status: 403, retryAfter: nil, rateLimitReset: "\(Int(now) - 10)", now: now) == .refused(retryAfter: ReleaseFeed.holdFloor),
      "reset in the past: the floor")
check(ReleaseFeed.classify(status: 403, retryAfter: "0", rateLimitReset: nil, now: now) == .refused(retryAfter: ReleaseFeed.holdFloor),
      "retry-after 0: the floor")
check(ReleaseFeed.classify(status: 403, retryAfter: nil, rateLimitReset: "\(Int(now) + 200_000)", now: now) == .refused(retryAfter: 3600),
      "reset days away: capped at an hour")
check(ReleaseFeed.classify(status: 429, retryAfter: "Wed, 21 Oct 2026 07:28:00 GMT", rateLimitReset: "garbage", now: now)
      == .refused(retryAfter: ReleaseFeed.refusedDefaultWait),
      "unparseable headers: the default wait")
check(ReleaseFeed.classify(status: 429, retryAfter: " 90 ", rateLimitReset: nil, now: now) == .refused(retryAfter: 90),
      "retry-after with whitespace: parsed")

if failures == 0 { print("releasefeed: all checks passed") } else { print("releasefeed: \(failures) FAILED"); exit(1) }
