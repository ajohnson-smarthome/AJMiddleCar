import SwiftUI

/// Edits one trick's per-action durations (slider 0.1–10 s each, per distinct movement).
struct TrickEditorView: View {
    let trick: Trick
    let palette: Palette
    @Environment(\.dismiss) private var dismiss
    @State private var durs: [Int] = []
    @State private var diameterCm = Tricks.donutDiaDefaultCm
    @State private var circles = Tricks.donutCirclesDefault
    @State private var spinTurns = Tricks.spinTurnsDefault
    @State private var spinDurMs = Tricks.spinDurDefaultMs
    @State private var fig8Dia = Tricks.fig8DiaDefaultCm
    @State private var fig8Eights = Tricks.fig8EightsDefault
    @State private var wiggleAmp = Tricks.wiggleAmpDefault
    @State private var wiggleWags = Tricks.wiggleWagsDefault
    private var p: Palette { palette }
    private var actions: [(t: Double, y: Double, count: Int)] { Tricks.distinctActions(trick) }
    private var totalSec: Double {
        Double(zip(actions, durs).reduce(0) { $0 + $1.1 * $1.0.count }) / 1000
    }

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                header
                if trick.id == Tricks.donut.id {
                    // One shared scroll: animation + stats + diameter + circle count scroll together.
                    ScrollView {
                        VStack(spacing: 16) {
                            TrickSimView(trick: Tricks.donut, durs: durs, palette: p,
                                         donutDiameterCm: Double(diameterCm), donutCircles: circles)
                            VStack(spacing: 0) {
                                diameterRow.padding(.horizontal, 14)
                                Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1)
                                circlesRow.padding(.horizontal, 14)
                            }
                            .background(p.panel)
                            .clipShape(RoundedRectangle(cornerRadius: 12))
                            .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.metal.opacity(0.4), lineWidth: 1))
                            .padding(.horizontal, 16)
                        }
                        .padding(.bottom, 16)
                    }
                } else if trick.id == Tricks.spin.id {
                    // One shared scroll: animation + stats + turns + duration scroll together.
                    ScrollView {
                        VStack(spacing: 16) {
                            TrickSimView(trick: Tricks.spin, durs: durs, palette: p,
                                         spinTurns: spinTurns, spinDurMs: spinDurMs)
                            VStack(spacing: 0) {
                                turnsRow.padding(.horizontal, 14)
                                Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1)
                                durationRow.padding(.horizontal, 14)
                            }
                            .background(p.panel)
                            .clipShape(RoundedRectangle(cornerRadius: 12))
                            .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.metal.opacity(0.4), lineWidth: 1))
                            .padding(.horizontal, 16)
                        }
                        .padding(.bottom, 16)
                    }
                } else if trick.id == Tricks.figure8.id {
                    // One shared scroll: animation + stats + loop diameter + eights count scroll together.
                    ScrollView {
                        VStack(spacing: 16) {
                            TrickSimView(trick: Tricks.figure8, durs: durs, palette: p,
                                         fig8Dia: Double(fig8Dia), fig8Eights: fig8Eights)
                            VStack(spacing: 0) {
                                fig8DiaRow.padding(.horizontal, 14)
                                Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1)
                                fig8EightsRow.padding(.horizontal, 14)
                            }
                            .background(p.panel)
                            .clipShape(RoundedRectangle(cornerRadius: 12))
                            .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.metal.opacity(0.4), lineWidth: 1))
                            .padding(.horizontal, 16)
                        }
                        .padding(.bottom, 16)
                    }
                } else if trick.id == Tricks.wiggle.id {
                    // One shared scroll: animation + stats + amplitude + wag count scroll together.
                    ScrollView {
                        VStack(spacing: 16) {
                            TrickSimView(trick: Tricks.wiggle, durs: durs, palette: p,
                                         wiggleAmp: wiggleAmp, wiggleWags: wiggleWags)
                            VStack(spacing: 0) {
                                wiggleAmpRow.padding(.horizontal, 14)
                                Rectangle().fill(p.metal.opacity(0.25)).frame(height: 1)
                                wiggleWagsRow.padding(.horizontal, 14)
                            }
                            .background(p.panel)
                            .clipShape(RoundedRectangle(cornerRadius: 12))
                            .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.metal.opacity(0.4), lineWidth: 1))
                            .padding(.horizontal, 16)
                        }
                        .padding(.bottom, 16)
                    }
                } else {
                    controls
                }
            }
        }
        .toolbar(.hidden, for: .navigationBar)
        .onAppear {
            if durs.isEmpty { durs = TrickSettings.durations(for: trick) }
            diameterCm = TrickSettings.donutDiameterCm()
            circles = TrickSettings.donutCircles()
            spinTurns = TrickSettings.spinTurns()
            spinDurMs = TrickSettings.spinDurMs()
            fig8Dia = TrickSettings.fig8Dia()
            fig8Eights = TrickSettings.fig8Eights()
            wiggleAmp = TrickSettings.wiggleAmp()
            wiggleWags = TrickSettings.wiggleWags()
        }
    }

    private var controls: some View {
        VStack(spacing: 0) {
            List {
                ForEach(actions.indices, id: \.self) { i in
                    row(i)
                        .listRowBackground(p.panel)
                        .alignmentGuide(.listRowSeparatorLeading) { _ in 0 }
                }
            }
            .scrollContentBackground(.hidden)
            .tint(p.accent)
            Text(L.trickTotal(totalSec))
                .font(.system(size: 12)).foregroundStyle(p.muted).monospacedDigit()
                .frame(maxWidth: .infinity)
                .frame(height: 44)
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Button { dismiss() } label: {
                Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold)).foregroundStyle(p.accent)
            }.buttonStyle(.plain)
            Text(L.trickName(trick.nameKey)).font(.system(size: 17, weight: .semibold)).foregroundStyle(p.text)
            Spacer()
        }
        .padding(.horizontal, 20).padding(.top, 12).padding(.bottom, 8)
    }

    @ViewBuilder private func row(_ i: Int) -> some View {
        let a = actions[i]
        let base = Tricks.baseDurations(trick)
        let isDefault = durs.indices.contains(i) && durs[i] == base[i]
        let secs = durs.indices.contains(i) ? Double(durs[i]) / 1000 : 0
        HStack(spacing: 11) {
            VStack(alignment: .leading, spacing: 1) {
                Text(actionLabel(Tricks.actionDescriptor(a.t, a.y))).font(.system(size: 13)).foregroundStyle(p.text)
                if a.count > 1 { Text(L.trickCycles(a.count)).font(.system(size: 9)).foregroundStyle(p.muted) }
            }
            .frame(width: 150, alignment: .leading)
            Slider(value: Binding(
                get: { secs },
                set: { if durs.indices.contains(i) { durs[i] = Int($0 * 1000) } }
            ), in: 0.1...10) { editing in
                if !editing, durs.indices.contains(i) { TrickSettings.setDuration(trick, action: i, ms: durs[i]) }
            }
            .tint(p.accent)
            Text(L.trickSec(secs)).font(.system(size: 13, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 54, alignment: .trailing)
            Button {
                durs[i] = base[i]; TrickSettings.reset(trick, action: i)
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var diameterRow: some View {
        let isDefault = diameterCm == Tricks.donutDiaDefaultCm
        HStack(spacing: 11) {
            Text(L.simDiameter).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Slider(value: Binding(
                get: { Double(diameterCm) },
                set: { diameterCm = Int(($0 / 5).rounded()) * 5 }
            ), in: Double(Tricks.donutDiaMinCm)...Double(Tricks.donutDiaMaxCm), step: 5) { editing in
                if !editing { TrickSettings.setDonutDiameter(diameterCm) }
            }
            .tint(p.accent)
            Text("\(diameterCm) \(L.cmUnit)").font(.system(size: 13, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 54, alignment: .trailing)
            Button {
                diameterCm = Tricks.donutDiaDefaultCm; TrickSettings.resetDonutDiameter()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var circlesRow: some View {
        let isDefault = circles == Tricks.donutCirclesDefault
        HStack(spacing: 11) {
            Text(L.simCircles).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Spacer()
            stepButton("minus") {
                circles = Swift.max(Tricks.donutCirclesMin, circles - 1); TrickSettings.setDonutCircles(circles)
            }.disabled(circles <= Tricks.donutCirclesMin)
            Text("\(circles)").font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 34)
            stepButton("plus") {
                circles = Swift.min(Tricks.donutCirclesMax, circles + 1); TrickSettings.setDonutCircles(circles)
            }.disabled(circles >= Tricks.donutCirclesMax)
            Button {
                circles = Tricks.donutCirclesDefault; TrickSettings.resetDonutCircles()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault).padding(.leading, 4)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var turnsRow: some View {
        let isDefault = spinTurns == Tricks.spinTurnsDefault
        HStack(spacing: 11) {
            Text(L.spinTurns).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Spacer()
            stepButton("minus") {
                spinTurns = Swift.max(Tricks.spinTurnsMin, spinTurns - 1); TrickSettings.setSpinTurns(spinTurns)
            }.disabled(spinTurns <= Tricks.spinTurnsMin)
            Text("\(spinTurns)").font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 34)
            stepButton("plus") {
                spinTurns = Swift.min(Tricks.spinTurnsMax, spinTurns + 1); TrickSettings.setSpinTurns(spinTurns)
            }.disabled(spinTurns >= Tricks.spinTurnsMax)
            Button {
                spinTurns = Tricks.spinTurnsDefault; TrickSettings.resetSpinTurns()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault).padding(.leading, 4)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var durationRow: some View {
        let isDefault = spinDurMs == Tricks.spinDurDefaultMs
        let durBinding = Binding<Double>(
            get: { Double(spinDurMs) / 1000 },
            set: { spinDurMs = Int(($0 * 2).rounded()) * 500 }   // 0.5 s steps
        )
        let durRange = Double(Tricks.spinDurMinMs) / 1000...Double(Tricks.spinDurMaxMs) / 1000
        HStack(spacing: 11) {
            Text(L.spinDuration).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Slider(value: durBinding, in: durRange, step: 0.5) { editing in
                if !editing { TrickSettings.setSpinDurMs(spinDurMs) }
            }
            .tint(p.accent)
            Text(L.trickSec(Double(spinDurMs) / 1000)).font(.system(size: 13, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 54, alignment: .trailing)
            Button {
                spinDurMs = Tricks.spinDurDefaultMs; TrickSettings.resetSpinDurMs()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var fig8DiaRow: some View {
        let isDefault = fig8Dia == Tricks.fig8DiaDefaultCm
        HStack(spacing: 11) {
            Text(L.fig8Diameter).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Spacer()
            stepButton("minus") {
                fig8Dia = Swift.max(Tricks.fig8DiaMinCm, fig8Dia - 10); TrickSettings.setFig8Dia(fig8Dia)
            }.disabled(fig8Dia <= Tricks.fig8DiaMinCm)
            Text("\(fig8Dia) \(L.cmUnit)").font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 56)
            stepButton("plus") {
                fig8Dia = Swift.min(Tricks.fig8DiaMaxCm, fig8Dia + 10); TrickSettings.setFig8Dia(fig8Dia)
            }.disabled(fig8Dia >= Tricks.fig8DiaMaxCm)
            Button {
                fig8Dia = Tricks.fig8DiaDefaultCm; TrickSettings.resetFig8Dia()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault).padding(.leading, 4)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var fig8EightsRow: some View {
        let isDefault = fig8Eights == Tricks.fig8EightsDefault
        HStack(spacing: 11) {
            Text(L.fig8Loops).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Spacer()
            stepButton("minus") {
                fig8Eights = Swift.max(Tricks.fig8EightsMin, fig8Eights - 1); TrickSettings.setFig8Eights(fig8Eights)
            }.disabled(fig8Eights <= Tricks.fig8EightsMin)
            Text("\(fig8Eights)").font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 34)
            stepButton("plus") {
                fig8Eights = Swift.min(Tricks.fig8EightsMax, fig8Eights + 1); TrickSettings.setFig8Eights(fig8Eights)
            }.disabled(fig8Eights >= Tricks.fig8EightsMax)
            Button {
                fig8Eights = Tricks.fig8EightsDefault; TrickSettings.resetFig8Eights()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault).padding(.leading, 4)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var wiggleAmpRow: some View {
        let isDefault = wiggleAmp == Tricks.wiggleAmpDefault
        let ampBinding = Binding<Double>(
            get: { wiggleAmp },
            set: { wiggleAmp = (($0 * 10).rounded()) / 10 }   // 0.1 steps
        )
        let ampRange = Tricks.wiggleAmpMin...Tricks.wiggleAmpMax
        HStack(spacing: 11) {
            Text(L.wiggleAmp).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Slider(value: ampBinding, in: ampRange, step: 0.1) { editing in
                if !editing { TrickSettings.setWiggleAmp(wiggleAmp) }
            }
            .tint(p.accent)
            Text(String(format: "%.1f", wiggleAmp)).font(.system(size: 13, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 54, alignment: .trailing)
            Button {
                wiggleAmp = Tricks.wiggleAmpDefault; TrickSettings.resetWiggleAmp()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var wiggleWagsRow: some View {
        let isDefault = wiggleWags == Tricks.wiggleWagsDefault
        HStack(spacing: 11) {
            Text(L.wiggleCount).font(.system(size: 13)).foregroundStyle(p.text)
                .frame(width: 150, alignment: .leading)
            Spacer()
            stepButton("minus") {
                wiggleWags = Swift.max(Tricks.wiggleWagsMin, wiggleWags - 1); TrickSettings.setWiggleWags(wiggleWags)
            }.disabled(wiggleWags <= Tricks.wiggleWagsMin)
            Text("\(wiggleWags)").font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).monospacedDigit().frame(width: 34)
            stepButton("plus") {
                wiggleWags = Swift.min(Tricks.wiggleWagsMax, wiggleWags + 1); TrickSettings.setWiggleWags(wiggleWags)
            }.disabled(wiggleWags >= Tricks.wiggleWagsMax)
            Button {
                wiggleWags = Tricks.wiggleWagsDefault; TrickSettings.resetWiggleWags()
            } label: {
                Image(systemName: "arrow.counterclockwise").font(.system(size: 13))
                    .foregroundStyle(isDefault ? p.muted : p.accent)
                    .frame(width: 28, height: 28)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(isDefault ? p.line : p.accent.opacity(0.4)))
            }
            .buttonStyle(.plain).disabled(isDefault).padding(.leading, 4)
        }
        .padding(.vertical, 4)
    }

    private func stepButton(_ symbol: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 15, weight: .semibold))
                .foregroundStyle(p.accent).frame(width: 36, height: 32)
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(p.accent.opacity(0.4)))
        }
        .buttonStyle(.plain)
    }

    private func actionLabel(_ d: (fwd: Int, turn: Int)) -> String {
        let dir = d.fwd > 0 ? L.actFwd : (d.fwd < 0 ? L.actBack : nil)
        let turn = d.turn > 0 ? L.actRight : (d.turn < 0 ? L.actLeft : nil)
        switch (dir, turn) {
        case let (dr?, tn?): return "\(dr)-\(tn)"
        case let (dr?, nil): return dr
        case let (nil, tn?): return "\(L.actTurn) \(tn)"
        default: return L.actFwd
        }
    }
}
