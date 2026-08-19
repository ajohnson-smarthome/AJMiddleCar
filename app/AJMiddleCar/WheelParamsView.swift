import SwiftUI

/// Wheel diameter + motor encoder params (PPR · gear · quadrature → CPR), stored on the car
/// via /wheel. Two uses: a Settings menu item (wizard == false, back chevron) and step 2 of
/// the mandatory calibration wizard (wizard == true, "Далее" → CalibrationView). No system
/// nav bar (matches SplitScreen siblings) — draws its own header.
struct WheelParamsView: View {
    let palette: Palette
    var wizard: Bool = false
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    @State private var diameterMm = 65
    @State private var ppr = 11
    @State private var gearX100 = 2100
    @State private var quad = 4
    @State private var gearText = "21"
    @State private var lastSaved: WheelClient.Params?

    private var preset: MotorPreset? { MotorPresets.match(ppr: ppr, gearX100: gearX100, quad: quad) }
    private var cpr: Double { MotorPresets.cpr(ppr: ppr, gearX100: gearX100, quad: quad) }
    private var circMm: Double { .pi * Double(diameterMm) }

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                header
                ScrollView {
                    VStack(spacing: 18) {
                        wheelsCard
                        motorsCard
                    }
                    .frame(maxWidth: 560)
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 20).padding(.top, 6).padding(.bottom, 20)
                }
            }
        }
        .toolbar(.hidden, for: .navigationBar)
        .task {
            if let c = await WheelClient().get() {
                diameterMm = c.diameterMm; ppr = c.ppr; gearX100 = c.gearX100; quad = c.quad
                gearText = Self.gearString(c.gearX100)
                lastSaved = c
            }
        }
    }

    // MARK: header
    private var header: some View {
        HStack {
            if wizard {
                Text(L.wheelStep(2, 3)).font(.system(size: 13)).foregroundStyle(p.muted)
                    .frame(width: 70, alignment: .leading)
            } else {
                Button { dismiss() } label: {
                    Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold))
                }
                .foregroundStyle(p.accent).frame(width: 70, alignment: .leading)
            }
            Spacer()
            Text(wizard ? L.wheelWizardTitle : L.wheelTitle)
                .font(.system(size: 17, weight: .semibold)).foregroundStyle(p.text)
            Spacer()
            Group {
                if wizard {
                    NavigationLink { CalibrationView(palette: p, dismissible: false) } label: {
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

    // MARK: cards
    private var wheelsCard: some View {
        card(L.wheelSectionWheels) {
            row(L.wheelDiameter) {
                Stepper("\(diameterMm) \(L.mmUnit)", value: $diameterMm, in: 20...150)
                    .fixedSize().foregroundStyle(p.text)
                    .onChange(of: diameterMm) { _ in save() }
            }
            divider
            infoRow(L.wheelCirc, String(format: "%.0f %@", circMm, L.mmUnit))
        }
    }

    private var motorsCard: some View {
        card(L.wheelSectionMotors) {
            row(L.wheelModel) {
                Menu {
                    ForEach(MotorPresets.all) { m in
                        Button { apply(m) } label: { Text("\(m.name) · \(m.rpm) \(L.rpmUnit)") }
                    }
                } label: {
                    HStack(spacing: 6) {
                        Text(preset?.name ?? L.wheelCustom).foregroundStyle(p.accent)
                        Image(systemName: "chevron.up.chevron.down")
                            .font(.system(size: 11)).foregroundStyle(p.muted)
                    }
                }
            }
            divider
            row(L.wheelPpr) {
                Stepper("\(ppr)", value: $ppr, in: 1...1000)
                    .fixedSize().foregroundStyle(p.text)
                    .onChange(of: ppr) { _ in save() }
            }
            divider
            row(L.wheelGear) {
                TextField("", text: $gearText)
                    .keyboardType(.decimalPad).multilineTextAlignment(.trailing)
                    .frame(width: 70).foregroundStyle(p.text)
                    .onChange(of: gearText) { _ in commitGear() }
            }
            divider
            row(L.wheelQuad) {
                Picker("", selection: $quad) {
                    Text("×1").tag(1); Text("×2").tag(2); Text("×4").tag(4)
                }
                .pickerStyle(.segmented).frame(width: 150)
                .onChange(of: quad) { _ in save() }
            }
            divider
            infoRow("CPR", String(format: "%.0f", cpr))
        }
    }

    // MARK: actions
    private func apply(_ m: MotorPreset) {
        ppr = m.ppr; gearX100 = m.gearX100; quad = m.quad
        gearText = Self.gearString(m.gearX100)
        save()
    }

    private func commitGear() {
        let norm = gearText.replacingOccurrences(of: ",", with: ".")
        if let g = Double(norm), g >= 1, g <= 300 {
            gearX100 = Int((g * 100).rounded())
            save()
        }
    }

    private func save() {
        let cur = WheelClient.Params(diameterMm: diameterMm, ppr: ppr,
                                     gearX100: gearX100, quad: quad)
        guard cur != lastSaved else { return }
        lastSaved = cur
        Task { await WheelClient().set(cur) }
    }

    static func gearString(_ x100: Int) -> String {
        if x100 % 100 == 0 { return String(x100 / 100) }
        if x100 % 10 == 0  { return String(format: "%.1f", Double(x100) / 100) }
        return String(format: "%.2f", Double(x100) / 100)
    }

    // MARK: row/card builders
    @ViewBuilder private func card<C: View>(_ title: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title.uppercased()).font(.system(size: 11, weight: .semibold))
                .foregroundStyle(p.muted).padding(.leading, 4).padding(.bottom, 6)
            VStack(spacing: 0) { content() }
                .background(p.panel)
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.metal.opacity(0.4), lineWidth: 1))
        }
    }

    @ViewBuilder private func row<C: View>(_ label: String, @ViewBuilder _ control: () -> C) -> some View {
        HStack { Text(label).foregroundStyle(p.text); Spacer(); control() }
            .font(.system(size: 14)).padding(.horizontal, 14).frame(minHeight: 44)
    }

    private func infoRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).foregroundStyle(p.muted)
            Spacer()
            Text(value).foregroundStyle(p.accent).fontWeight(.semibold).monospacedDigit()
        }
        .font(.system(size: 14)).padding(.horizontal, 14).frame(minHeight: 44)
    }

    private var divider: some View { Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1) }
}
