// Host test for VideoGauge — what the picture's instrument shows for the receiver's counters
// (openspec `app/drive-hud`, «Приборы показывают связь и намерение пульта», the picture's
// scenarios «Картинка без потерь» and «Кадры теряются»; «Показ — кадр за кадром», «Показания»).
//
// The instrument is arithmetic over two numbers: the caption «N к/с», whether the loss pair is
// shown at all (only while something was lost), and the one sentence VoiceOver reads for the
// whole instrument. The words come from the caller — `L` in the app, the spec's here, since a
// host binary has no `Localizable.strings` — so what is tested is the rule, not the bundle.
// The view (`VideoBadge`) only draws what this says.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let words = VideoGauge.Words(stats: { "\($0) к/с" }, lost: { "потеряно \($0)" })

// «Картинка без потерь»: one pair, and not a word about losses — on screen or for VoiceOver.
do {
    let g = VideoGauge.make(fps: 25, lost: 0, words: words)
    check(g.caption == "25 к/с", "no loss: the caption is «25 к/с»")
    check(g.lost == nil, "no loss: the loss pair is not shown")
    check(g.accessibility == "25 к/с", "no loss: VoiceOver reads the caption alone")
}

// «Кадры теряются»: the caption unchanged, the loss pair with its number, and VoiceOver told.
do {
    let g = VideoGauge.make(fps: 22, lost: 7, words: words)
    check(g.caption == "22 к/с", "loss: the caption is still «N к/с»")
    check(g.lost == 7, "loss: the pair shows the count")
    check(g.accessibility == "22 к/с, потеряно 7", "loss: VoiceOver reads «N к/с, потеряно M»")
}

// A picture that has just appeared, or a stalled one: zero is a number, not an empty caption.
check(VideoGauge.make(fps: 0, lost: 0, words: words).caption == "0 к/с", "0 fps: «0 к/с»")

// One lost frame is a loss — the pair appears at the first one, not at some threshold.
check(VideoGauge.make(fps: 24, lost: 1, words: words).lost == 1, "one lost frame: the pair is shown")

// The receiver never publishes a negative count (`VideoLink` clamps), but the instrument does
// not show one either — it is «no losses», the same as zero.
check(VideoGauge.make(fps: 24, lost: -3, words: words).lost == nil, "negative count: as no loss")

if failures == 0 { print("videogauge: ok") } else { exit(1) }
