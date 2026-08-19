import SwiftUI

/// Center-anchored segmented power meter for one side. value ∈ [-1, 1]:
/// forward (>0) lights green segments upward from the centre, reverse (<0) lights amber downward.
struct PowerBar: View {
    let value: Double
    let palette: Palette

    private let count = 5
    private var off: Color { palette.segOff }

    var body: some View {
        let lit = min(count, Int((abs(value) * Double(count)).rounded()))
        let fwd = value > 0.03
        let rev = value < -0.03
        VStack(spacing: 3) {
            ForEach(0..<count, id: \.self) { i in        // up = forward, lit from centre out
                seg(on: fwd && (count - i) <= lit, color: palette.accent)
            }
            RoundedRectangle(cornerRadius: 1).fill(palette.line).frame(width: 22, height: 2)
            ForEach(0..<count, id: \.self) { i in        // down = reverse, lit from centre out
                seg(on: rev && (i + 1) <= lit, color: palette.warn)
            }
        }
    }

    private func seg(on: Bool, color: Color) -> some View {
        RoundedRectangle(cornerRadius: 2)
            .fill(on ? color : off)
            .frame(width: 18, height: 6)
            .shadow(color: on ? color.opacity(0.5) : .clear, radius: on ? 3 : 0)
    }
}
