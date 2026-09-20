import Foundation

/// The battery badge as arithmetic (openspec `app/drive-hud`, «Приборы показывают связь и
/// намерение пульта»): what the icon and its caption show for the `battery` group of the last
/// telemetry frame (`car/battery-monitor`). Pure and host-tested (`app/tests/batterygauge`);
/// `BatteryBadge` draws exactly this and decides nothing of its own.
struct BatteryGauge: Equatable {
    /// Which of the palette's colours the whole badge wears: `text` while the monitor answers,
    /// `warn` once the pack is `low`, `muted` when there is no monitor to ask.
    enum Tone: Equatable { case text, warn, muted }

    /// The caption's numbers. `pct` is `nil` while the car has not determined the start yet —
    /// volts and watts are measured, the percent is not (the icon is empty then too).
    struct Stats: Equatable {
        let pct: Int?
        /// One decimal, rounded — «12,3», with the decimal comma of the app's one language.
        let volts: String
        let watts: Int
    }

    /// The icon's fill, `0...1`; `nil` is an empty icon — no monitor, or no `soc_pct` yet.
    let fill: Double?
    /// Current flowing *into* the pack (`current_ma` negative): the bolt on the icon.
    let charging: Bool
    let tone: Tone
    /// `nil` is «—» in place of every number: the monitor is absent.
    let stats: Stats?

    /// No frame at all reads as no monitor: nothing to say, and nothing to warn about.
    static func make(_ b: BatteryInfo?) -> BatteryGauge {
        guard let b, b.state != .absent else {
            return BatteryGauge(fill: nil, charging: false, tone: .muted, stats: nil)
        }
        let fill = b.soc_pct.map { Double(min(100, max(0, $0))) / 100 }
        // `low` is the one word that changes the colour; anything else the car says — `ok`, or a
        // word this build does not know — shows its numbers in the plain tone.
        let tone: Tone = b.state == .low ? .warn : .text
        // Both numbers are measured together; either missing while the monitor answers is a car
        // this build does not understand, and «—» is more honest than half a caption.
        let stats: Stats? = {
            guard let mv = b.voltage_mv, let mw = b.power_mw else { return nil }
            let tenths = Int((Double(mv) / 100).rounded())
            return Stats(pct: b.soc_pct, volts: "\(tenths / 10),\(tenths % 10)",
                         watts: Int((Double(mw) / 1000).rounded()))
        }()
        return BatteryGauge(fill: fill, charging: (b.current_ma ?? 0) < 0, tone: tone, stats: stats)
    }
}
