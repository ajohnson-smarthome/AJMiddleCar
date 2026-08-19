import SwiftUI

struct JoystickView: View {
    var vertical: Bool = false
    var size: CGFloat = 122
    var palette: Palette
    var onChange: (Double, Double) -> Void

    @State private var knob: CGSize = .zero

    var body: some View {
        ZStack {
            Circle().fill(palette.panel).overlay(Circle().strokeBorder(palette.line))
            Circle().fill(palette.accent).frame(width: 50, height: 50).offset(knob)
        }
        .frame(width: size, height: size)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { g in
                    let r = size / 2
                    var dx = g.location.x - r
                    var dy = g.location.y - r
                    if vertical { dx = 0 }
                    let d = (dx * dx + dy * dy).squareRoot()
                    if d > r { dx = dx / d * r; dy = dy / d * r }
                    knob = CGSize(width: dx, height: dy)
                    onChange(Double(dx / r), Double(dy / r))
                }
                .onEnded { _ in knob = .zero; onChange(0, 0) }
        )
    }
}
