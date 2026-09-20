import SwiftUI

/// The control-scheme switch — «Аркада» · «Танк» — in the drive screen's top row, left of the
/// control bar. The colours and the remembered choice are `app/screens-and-theme`'s; the shape
/// is `app/drive-hud`'s: a pill of `HudPill.height` and `HudPill.radius`, the same as the bar
/// beside it, so the two stand on one pair of lines. The body is clear with a `line` stroke,
/// the chosen segment on `panel`.
struct SchemeToggle: View {
    @Binding var scheme: String
    let palette: Palette

    var body: some View {
        HStack(spacing: 0) {
            seg(L.schemeArcade, "arcade")
            seg(L.schemeTank, "tank")
        }
        .clipShape(RoundedRectangle(cornerRadius: HudPill.radius))
        .overlay(RoundedRectangle(cornerRadius: HudPill.radius).stroke(palette.line))
    }

    private func seg(_ label: String, _ value: String) -> some View {
        Text(label)
            .font(.system(size: 13))
            .padding(.horizontal, 13)
            .frame(height: HudPill.height)
            .foregroundStyle(scheme == value ? palette.accent : palette.muted)
            .background(scheme == value ? palette.panel : Color.clear)
            .contentShape(Rectangle())
            .onTapGesture { scheme = value }
    }
}
