import SwiftUI

/// Four ascending signal bars, filled up to `level` (0...4).
struct SignalBars: View {
    let level: Int
    let color: Color
    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(1...4, id: \.self) { i in
                RoundedRectangle(cornerRadius: 1)
                    .fill(i <= level ? color : color.opacity(0.18))
                    .frame(width: 3, height: CGFloat(2 + i * 3))
            }
        }
        .frame(height: 14)
    }
}
