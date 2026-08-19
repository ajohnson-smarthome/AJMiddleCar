import SwiftUI

/// «Размеры машинки» — track + wheelbase between wheel centres, stored on the car via /dims.
/// Two uses: a Settings menu item (wizard == false, back chevron) and step 1 of the mandatory
/// calibration wizard (wizard == true, "Далее" → WheelParamsView). No system nav bar (matches
/// SplitScreen siblings) — draws its own header. The track feeds the donut/simulation math.
struct CarDimensionsView: View {
    let palette: Palette
    var wizard: Bool = false
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    @State private var trackMm = 130
    @State private var wheelbaseMm = 210
    @State private var lastSaved: DimsClient.Params?

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                header
                ScrollView {
                    VStack(spacing: 18) {
                        CarDimsDiagram(trackMm: trackMm, wheelbaseMm: wheelbaseMm, palette: p)
                            .padding(.top, 4)
                        card
                    }
                    .frame(maxWidth: 560)
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 20).padding(.top, 6).padding(.bottom, 20)
                }
            }
        }
        .toolbar(.hidden, for: .navigationBar)
        .task {
            if let d = await DimsClient().get() {
                trackMm = d.trackMm; wheelbaseMm = d.wheelbaseMm; lastSaved = d
            }
        }
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
            stepperRow(L.dimsTrack, L.dimsTrackHint, value: $trackMm, range: 60...300)
            Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1)
            stepperRow(L.dimsBase, L.dimsBaseHint, value: $wheelbaseMm, range: 90...360)
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

    private func save() {
        let pms = DimsClient.Params(trackMm: trackMm, wheelbaseMm: wheelbaseMm)
        guard pms != lastSaved else { return }
        lastSaved = pms
        Task { await DimsClient().set(pms) }
    }
}
