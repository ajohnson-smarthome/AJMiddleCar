import SwiftUI

/// What a configuration domain renders when the app has not managed to read it.
///
/// It exists so that "not read" can never be drawn as a number. A settings screen that shows its
/// own fallback as if it were the car's configuration invites the user to nudge one control and
/// POST the whole record back — which is how the car's real gear ratio used to get overwritten
/// with 2100 by a single stepper tap.
struct ConfigNotice: View {
    let palette: Palette
    var error: CarError?
    let onRetry: () -> Void
    private var p: Palette { palette }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "questionmark.circle").foregroundStyle(p.warn)
                Text(L.configNotRead).foregroundStyle(p.warn)
            }
            .font(.system(size: 14, weight: .semibold))
            Button(action: onRetry) {
                Text(L.configRetry)
                    .font(.system(size: 14, weight: .semibold)).foregroundStyle(p.accent)
                    .padding(.horizontal, 16).padding(.vertical, 8)
                    .background(RoundedRectangle(cornerRadius: 10).fill(p.accent.opacity(0.15)))
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(p.accent.opacity(0.55), lineWidth: 1))
            }
            .buttonStyle(.plain)
        }
        .frame(maxWidth: 260, alignment: .leading)
    }
}
