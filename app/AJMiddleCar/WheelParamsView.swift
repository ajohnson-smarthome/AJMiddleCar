import SwiftUI

/// Wheel diameter + motor encoder params (PPR · gear · quadrature → CPR), stored on the car
/// via /wheel. Two uses: a Settings menu item (wizard == false, back chevron) and step 2 of
/// the mandatory calibration wizard (wizard == true, "Далее" → CalibrationView). No system
/// nav bar (matches SplitScreen siblings) — draws its own header.
///
/// The cards appear only once the car's own values have been read. They used to be drawn from
/// the app's fallback whether or not the GET landed, so a single stepper tap POSTed 65/11/2100/4
/// over whatever the car actually had.
///
/// Every write is a control's own doing — a stepper's setter, the picker's, the preset menu,
/// the gear field finishing — never an `.onChange` of the state behind a control, which could
/// not tell the car's value being adopted from the user's hand (AJM-106). The gear ratio is
/// the one typed field, and it writes once, when the input is over (`GearEntry`, AJM-112).
struct WheelParamsView: View {
    let palette: Palette
    var wizard: Bool = false
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    @ObservedObject private var store = ConfigStore.shared.wheel
    @State private var diameterMm: Int
    @State private var ppr: Int
    @State private var gearX100: Int
    @State private var quad: Int
    @State private var gear: GearEntry
    @FocusState private var gearFocused: Bool

    init(palette: Palette, wizard: Bool = false) {
        self.palette = palette
        self.wizard = wizard
        // The first frame is the car's value when it is already read — not the app's default
        // for a frame (AJM-106); an unread domain draws no cards at all.
        let w = ConfigStore.shared.wheel.value ?? .default
        _diameterMm = State(initialValue: w.diameter_mm)
        _ppr = State(initialValue: w.encoder_ppr)
        _gearX100 = State(initialValue: Int((w.gear_ratio * 100).rounded()))
        _quad = State(initialValue: w.quadrature)
        _gear = State(initialValue: GearEntry(ratio: w.gear_ratio))
    }

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
                        if store.value != nil {
                            wheelsCard
                            motorsCard
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
        // The decimal pad has no return key: this is the gear field's «Готово».
        .toolbar {
            ToolbarItemGroup(placement: .keyboard) {
                Spacer()
                Button(L.done) { gearFocused = false }.foregroundStyle(p.accent)
            }
        }
        .task { await store.loadIfNeeded(); adopt() }
        // The input is over when the focus leaves the field — «Готово», a tap elsewhere — or
        // the screen is left with it still focused («Далее» in the wizard, the back chevron).
        .onChange(of: gearFocused) { _, focused in if !focused { commitGear() } }
        .onDisappear { commitGear() }
    }

    /// The car's value into the controls. Not a write.
    private func adopt() {
        guard let w = store.value else { return }
        diameterMm = w.diameter_mm
        ppr = w.encoder_ppr
        gearX100 = Int((w.gear_ratio * 100).rounded())
        quad = w.quadrature
        gear.adopt(w.gear_ratio)
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
                Stepper("\(diameterMm) \(L.mmUnit)", value: Binding(
                    get: { diameterMm },
                    set: { diameterMm = $0; save() }   // the tap, and only that, writes
                ), in: Wheel.diameter_mmRange)
                    .fixedSize().foregroundStyle(p.text)
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
                Stepper("\(ppr)", value: Binding(
                    get: { ppr },
                    set: { ppr = $0; save() }
                ), in: Wheel.encoder_pprRange)
                    .fixedSize().foregroundStyle(p.text)
            }
            divider
            row(L.wheelGear) {
                // Keystrokes only move the text and its tint; the number goes to the car when
                // the input is over (`commitGear`), not per character (AJM-112).
                TextField("", text: Binding(
                    get: { gear.text },
                    set: { gear.typed($0) }
                ))
                    .keyboardType(.decimalPad).multilineTextAlignment(.trailing)
                    .frame(width: 70).foregroundStyle(gear.valid ? p.text : p.warn)
                    .focused($gearFocused)
                    .onSubmit { gearFocused = false }
            }
            divider
            row(L.wheelQuad) {
                Picker("", selection: Binding(
                    get: { quad },
                    set: { quad = $0; save() }
                )) {
                    ForEach(Wheel.quadratureAllowed, id: \.self) { q in Text("×\(q)").tag(q) }
                }
                .pickerStyle(.segmented).frame(width: 150)
            }
            divider
            infoRow("CPR", String(format: "%.0f", cpr))
        }
    }

    // MARK: actions
    private func apply(_ m: MotorPreset) {
        ppr = m.ppr; gearX100 = m.gearX100; quad = m.quad
        gear.adopt(Double(m.gearX100) / 100)
        save()
    }

    /// The gear input is over: one write if the text meant a new number; otherwise the field
    /// is back on the car's ratio and nothing is sent.
    private func commitGear() {
        guard let g = gear.finished() else { return }
        gearX100 = Int((g * 100).rounded())
        save()
    }

    /// Save-dedup and the "never write what we did not read" rule both live in the store.
    private func save() {
        Task {
            await store.save(Wheel(diameter_mm: diameterMm, encoder_ppr: ppr,
                                   gear_ratio: Double(gearX100) / 100, quadrature: quad))
        }
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
