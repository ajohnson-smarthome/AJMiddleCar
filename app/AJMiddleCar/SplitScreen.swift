import SwiftUI

/// Shared split layout: car/graphic on the left, text panel on the right, centred
/// identically on every screen. Suppresses the system nav bar so no screen gets a
/// nav-bar inset (the source of the vertical misalignment); draws an optional custom
/// header (back chevron + title) as a top overlay instead.
struct SplitScreen<Left: View, Right: View>: View {
    let palette: Palette
    var title: String? = nil
    var onBack: (() -> Void)? = nil
    @ViewBuilder var left: () -> Left
    @ViewBuilder var right: () -> Right

    private var p: Palette { palette }

    var body: some View {
        ZStack(alignment: .topLeading) {
            p.bg.ignoresSafeArea()
            HStack(spacing: 24) {
                left().frame(maxWidth: .infinity, maxHeight: .infinity)
                right().frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(20)
            if title != nil || onBack != nil { header }
        }
        .toolbar(.hidden, for: .navigationBar)
    }

    private var header: some View {
        HStack(spacing: 8) {
            if let onBack {
                Button(action: onBack) {
                    Image(systemName: "chevron.left")
                        .font(.system(size: 17, weight: .semibold)).foregroundStyle(p.accent)
                }.buttonStyle(.plain)
            }
            if let title {
                Text(title).font(.system(size: 17, weight: .semibold)).foregroundStyle(p.text)
            }
            Spacer()
        }
        .padding(.horizontal, 20).padding(.top, 12)
    }
}
