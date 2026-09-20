import Foundation

/// The picture's instrument as arithmetic (openspec `app/drive-hud`, «Приборы показывают связь
/// и намерение пульта»): what the top row shows for the receiver's two counters — frames shown
/// in the last second, frames lost in the last ten. Pure and host-tested
/// (`app/tests/videogauge`); `VideoBadge` draws exactly this and decides nothing of its own.
///
/// The words are the caller's (`Words`): `L.videoStats(fps:)` and `L.videoLost(_:)` in the
/// app, the spec's own in the host test — a host binary has no `Localizable.strings`, and the
/// rule is what is under test, not the bundle. Same shape as `FlashRefusal.quote(phrase:)`.
struct VideoGauge: Equatable {
    /// «N к/с» and «потеряно M» — the two phrases the caption and the VoiceOver label are made of.
    struct Words {
        let stats: (Int) -> String
        let lost: (Int) -> String
    }

    /// «N к/с» — the one pair the instrument always shows.
    let caption: String
    /// The loss pair's number, or `nil` while nothing was lost: the pair is then not drawn at
    /// all, and the instrument is a single pair — the battery's silence until `low`, here until
    /// a frame goes missing.
    let lost: Int?
    /// What VoiceOver reads for the whole instrument: the caption, and «, потеряно M» after it
    /// only when the pair is shown — the word «потеряно» never reaches the row itself.
    let accessibility: String

    static func make(fps: Int, lost: Int, words: Words) -> VideoGauge {
        let caption = words.stats(fps)
        // The receiver clamps at zero already (`VideoLink.publishStats`); below it is still
        // «nothing lost» here rather than a pair with a minus sign.
        let shown: Int? = lost > 0 ? lost : nil
        return VideoGauge(caption: caption, lost: shown,
                          accessibility: shown.map { "\(caption), \(words.lost($0))" } ?? caption)
    }
}
