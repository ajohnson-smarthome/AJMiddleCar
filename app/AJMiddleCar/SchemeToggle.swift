import SwiftUI

struct SchemeToggle: View {
    @Binding var scheme: String
    let palette: Palette

    var body: some View {
        HStack(spacing: 0) {
            seg(L.schemeArcade, "arcade")
            seg(L.schemeTank, "tank")
        }
        .clipShape(RoundedRectangle(cornerRadius: 9))
        .overlay(RoundedRectangle(cornerRadius: 9).stroke(palette.line))
    }

    private func seg(_ label: String, _ value: String) -> some View {
        Text(label)
            .font(.system(size: 13))
            .padding(.horizontal, 13).padding(.vertical, 6)
            .foregroundStyle(scheme == value ? palette.accent : palette.muted)
            .background(scheme == value ? palette.panel : Color.clear)
            .contentShape(Rectangle())
            .onTapGesture { scheme = value }
    }
}
