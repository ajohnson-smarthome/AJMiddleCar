import SwiftUI

struct CalibrationView: View {
    let palette: Palette
    enum CalDebug { case spin, direction, done, saving, failed }   // gallery preview seed
    var debugState: CalDebug? = nil
    var dismissible: Bool = true   // Settings push = back chevron; mandatory auto-prompt = none
    @Environment(\.dismiss) private var dismiss

    @State private var step = 0
    @State private var assign: [Corner: (pair: Int, sign: Int)] = [:]
    @State private var pending: Corner?
    @State private var saving = false
    @State private var failed = false
    private let client = CalibClient()

    private var metal: Color { palette.metal }

    private enum CalState { case spin, direction, done, saving, failed }
    private var state: CalState {
        if saving { return .saving }
        if failed { return .failed }
        if pending != nil { return .direction }
        if step >= 4 { return .done }
        return .spin
    }
    private var ringsActive: Bool { state == .spin || state == .saving }
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p, title: L.calibTitle, onBack: dismissible ? { dismiss() } : nil) {
            carDiagram
        } right: {
            rightPanel
        }
        .onAppear {
            guard let d = debugState else { return }
            switch d {
            case .spin:      step = 0; pending = nil; saving = false; failed = false
            case .direction: pending = Corner.allCases.first
            case .done:      step = 4
            case .saving:    saving = true
            case .failed:    failed = true
            }
        }
    }

    // MARK: left — car (1:1 reference) + interactive wheels + pulse rings
    private var carDiagram: some View {
        TimelineView(.animation) { ctx in
            let t = ctx.date.timeIntervalSinceReferenceDate
            let ringS = 1.0 + 0.07 * (0.5 + 0.5 * sin(t * 2 * .pi / 1.4))
            let glow = 0.5 + 0.5 * sin(t * 2 * .pi / 1.0)
            ZStack {
                // Rings drawn in a single Canvas (one GPU layer, no per-frame layout → no jitter),
                // behind the car. Fixed 200×240 frame keeps the ZStack size stable whether rings
                // show or not — otherwise the wheels would re-centre on tap ("fly in").
                Canvas { gc, size in
                    guard ringsActive else { return }
                    let c = CGPoint(x: size.width / 2, y: size.height / 2)
                    let op: [Double] = [0.42, 0.24, 0.11]
                    for i in 0..<3 {
                        let r = CGFloat(56 + i * 24) / 2 * CGFloat(ringS)
                        let rect = CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)
                        gc.stroke(Path(ellipseIn: rect), with: .color(p.accent.opacity(op[i])), lineWidth: 1.5)
                    }
                }
                .frame(width: 200, height: 240)
                carBody
                ForEach(Corner.allCases, id: \.self) { wheelButton($0, glow: glow) }
            }
        }
        .scaleEffect(1.6)
        .frame(width: 200, height: 240)
        .transaction { $0.animation = nil }   // no implicit animation on tap → wheels don't "fly in"
    }

    private var carBody: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10).fill(p.bg)
                .overlay(RoundedRectangle(cornerRadius: 10).fill(p.panel))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(metal, lineWidth: 1))
                .frame(width: 34, height: 72)
            RoundedRectangle(cornerRadius: 3).fill(p.bg)
                .frame(width: 20, height: 8).offset(y: -25)
        }
    }

    private func wheelFill(_ c: Corner) -> Color {
        if state == .failed { return p.warn }
        if assign[c] != nil { return p.accent }
        if pending == c { return p.warn }
        return metal
    }
    private func wheelGlyph(_ c: Corner) -> String {
        if state == .failed { return "✕" }
        if assign[c] != nil { return "✓" }
        return ""
    }
    private func wheelButton(_ c: Corner, glow: Double) -> some View {
        Button { tap(c) } label: {
            Text(wheelGlyph(c))
                .font(.system(size: 8, weight: .heavy))
                .foregroundStyle(p.bg)
                .frame(width: 11, height: 15)
                .background(RoundedRectangle(cornerRadius: 3).fill(wheelFill(c)))
                .shadow(color: pending == c ? p.warn.opacity(0.9) : .clear,
                        radius: pending == c ? 2 + 4 * glow : 0)
        }
        .buttonStyle(.plain)
        .disabled(assign[c] != nil || state == .saving || state == .failed)
        .offset(x: c.dx, y: c.dy)
    }

    // MARK: right — unified template
    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 9) {
            segments
            switch state {
            case .spin:
                title(L.calibStep(min(step + 1, 4))); sub(L.calibSpinSub)
                pill(L.calibSpin, p.accent) { spin() }
            case .direction:
                if let c = pending {
                    title(L.calibWheel(c.label)); sub(L.calibWhichDir2)
                    HStack(spacing: 8) {
                        pill(L.calibForward, p.accent) { assignDir(1) }
                        pill(L.calibBack, p.warn) { assignDir(-1) }
                    }
                }
            case .done:
                title(L.calibDoneTitle); sub(L.calibAllSet)
                pill(L.calibSave, p.accent) { save() }
            case .saving:
                title(L.calibSaving); sub(L.calibSavingSub)
            case .failed:
                title(L.calibFailTitle); sub(L.calibFailSub)
                pill(L.calibRetry, p.accent) { failed = false; save() }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
    }

    private var segments: some View {
        HStack(spacing: 5) {
            ForEach(0..<4, id: \.self) { i in
                RoundedRectangle(cornerRadius: 2)
                    .fill((i <= step && step < 4) || i < step ? p.accent : p.line)
                    .frame(width: 26, height: 4)
                    .shadow(color: i == step && step < 4 ? p.accent.opacity(0.8) : .clear, radius: 4)
            }
        }
    }

    private func title(_ t: String) -> some View {
        Text(t).font(.system(size: 18, weight: .semibold)).foregroundStyle(p.text)
    }
    private func sub(_ t: String) -> some View {
        Text(t).font(.system(size: 12)).foregroundStyle(p.muted).fixedSize(horizontal: false, vertical: true)
    }
    private func pill(_ text: String, _ tint: Color, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(text).font(.system(size: 14, weight: .semibold)).foregroundStyle(tint)
                .padding(.horizontal, 16).padding(.vertical, 8)
                .background(RoundedRectangle(cornerRadius: 10).fill(tint.opacity(0.15)))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(tint.opacity(0.55), lineWidth: 1))
        }
        .buttonStyle(.plain).padding(.top, 2)
    }

    // MARK: logic (unchanged behavior)
    private func spin() { Task { await client.spin(pair: step, dir: 1) } }
    private func tap(_ c: Corner) { guard assign[c] == nil else { return }; pending = c }
    private func assignDir(_ sign: Int) {
        guard let c = pending else { return }
        assign[c] = (pair: step, sign: sign)
        pending = nil
        step += 1
    }
    private func save() {
        saving = true; failed = false
        Task {
            let ok = await client.save(body: ControlModel.calibSaveBody(assign))
            saving = false
            if ok { dismiss() } else { failed = true }
        }
    }
}

private extension Corner {
    var label: String { rawValue.uppercased() }
    var dx: CGFloat { (self == .fl || self == .rl) ? -18.5 : 18.5 }
    var dy: CGFloat { (self == .fl || self == .fr) ? -20.5 : 20.5 }
}
