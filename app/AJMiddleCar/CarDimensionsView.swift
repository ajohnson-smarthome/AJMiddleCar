import SwiftUI

/// «Размеры машинки» — track + wheelbase between wheel centres, stored on the car via /dims.
/// Two uses: a Settings menu item (wizard == false, back chevron) and step 1 of the mandatory
/// calibration wizard (wizard == true, "Далее" → WheelParamsView). No system nav bar (matches
/// SplitScreen siblings) — draws its own header. The track feeds the donut/simulation math.
///
/// The diagram and the steppers appear only once the car's own dimensions have been read: drawing
/// the app's fallback as if it were the car's measurement is what invites a tap that writes it.
struct CarDimensionsView: View {
    let palette: Palette
    var wizard: Bool = false
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    @ObservedObject private var store = ConfigStore.shared.dims
    @State private var trackMm = Dims.default.track_mm
    @State private var wheelbaseMm = Dims.default.wheelbase_mm

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                header
                ScrollView {
                    VStack(spacing: 18) {
                        if store.value != nil {
                            CarDimsDiagram(trackMm: trackMm, wheelbaseMm: wheelbaseMm, palette: p)
                                .padding(.top, 4)
                            card
                        } else {
                            ConfigNotice(palette: p, error: store.error) {
                                Task { await store.reload(); adopt() }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.top, 20)
                        }
                    }
                    .frame(maxWidth: 560)
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 20).padding(.top, 6).padding(.bottom, 20)
                }
            }
        }
        .toolbar(.hidden, for: .navigationBar)
        .task { await store.loadIfNeeded(); adopt() }
    }

    private func adopt() {
        guard let d = store.value else { return }
        trackMm = d.track_mm
        wheelbaseMm = d.wheelbase_mm
    }

    private var header: some View {
        HStack {
            if wizard {
                Text(L.wheelStep(1, 3)).font(.system(size: 13)).foregroundStyle(p.muted)
                    .frame(width: 70, alignment: .leading)
            } else {
                Button { dismiss() } label: {
                    Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold))
                }
                .foregroundStyle(p.accent).frame(width: 70, alignment: .leading)
            }
            Spacer()
            Text(L.dimsTitle).font(.system(size: 17, weight: .semibold)).foregroundStyle(p.text)
            Spacer()
            Group {
                if wizard {
                    NavigationLink { WheelParamsView(palette: p, wizard: true) } label: {
                        Text(L.wheelNext).font(.system(size: 16, weight: .semibold))
                    }
                    .foregroundStyle(p.accent)
                } else {
                    Color.clear.frame(width: 70, height: 1)
                }
            }
            .frame(width: 70, alignment: .trailing)
        }
        .padding(.horizontal, 20).padding(.top, 14).padding(.bottom, 8)
    }

    private var card: some View {
        VStack(spacing: 0) {
            stepperRow(L.dimsTrack, L.dimsTrackHint, value: $trackMm, range: Dims.track_mmRange)
            Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1)
            stepperRow(L.dimsBase, L.dimsBaseHint, value: $wheelbaseMm, range: Dims.wheelbase_mmRange)
        }
        .background(p.panel)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.metal.opacity(0.4), lineWidth: 1))
    }

    private func stepperRow(_ title: String, _ hint: String, value: Binding<Int>, range: ClosedRange<Int>) -> some View {
        HStack(spacing: 11) {
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 14)).foregroundStyle(p.text)
                Text(hint).font(.system(size: 11)).foregroundStyle(p.muted)
            }
            Spacer()
            stepButton("minus") { value.wrappedValue = Swift.max(range.lowerBound, value.wrappedValue - 5); save() }
                .disabled(value.wrappedValue <= range.lowerBound)
            Text("\(value.wrappedValue) \(L.mmUnit)").font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 72)
            stepButton("plus") { value.wrappedValue = Swift.min(range.upperBound, value.wrappedValue + 5); save() }
                .disabled(value.wrappedValue >= range.upperBound)
        }
        .padding(.horizontal, 14).padding(.vertical, 12)
    }

    private func stepButton(_ symbol: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).frame(width: 38, height: 32)
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(p.accent.opacity(0.4)))
        }
        .buttonStyle(.plain)
    }

    /// Save-dedup and the "never write what we did not read" rule both live in the store.
    private func save() {
        Task { await store.save(Dims(track_mm: trackMm, wheelbase_mm: wheelbaseMm)) }
    }
}
