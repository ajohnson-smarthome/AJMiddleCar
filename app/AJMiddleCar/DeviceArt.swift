import SwiftUI

/// The visual vocabulary every device screen is built from.
///
/// Before this file there were two of everything. `FirmwareCarView` drew the car from SwiftUI
/// shapes with rings at diameters 56/80/104; `ConnectCarView` drew a second car into a `Canvas`
/// with rings at radii 46/60/74. Same object, same idea, two sizes and two implementations — and
/// the startup sequence, which crosses between them, made the mismatch visible: the car changed
/// size as the screen changed, which reads as a jolt rather than as progress.
///
/// So the parts live here once. A screen is now a composition of three of them — rings behind,
/// an opaque object on top, and a chip badge carrying the state — and that composition is the
/// whole language: `RingMode` says what is happening around the device, the object's own opacity
/// says whether it has been found, and the chip says what it is busy with. Six startup screens
/// need nothing beyond those three dials.
enum DeviceArt {
    /// Rings that belong to the device itself — an update arriving, work in progress. They sit
    /// close, because what they describe is happening *inside* the thing they surround.
    static let ringD: [CGFloat] = [56, 80, 104]
    /// Rings that describe the air around the device, for the radar. Wider on purpose: this set
    /// is the space being searched, not the device, and the sweep needs room to read as a sweep.
    /// Collapsing the two sets into one was tried and looked worse — the beam had nowhere to go.
    static let fieldD: [CGFloat] = [92, 120, 148]
    /// Every device scene is drawn in this box at this scale, so an object keeps its size when
    /// the screen changes underneath it.
    static let stage = CGSize(width: 200, height: 240)
    static let scale: CGFloat = 1.6
}

/// What the rings are doing — the only thing that distinguishes most of the startup screens.
enum RingMode: Equatable {
    /// No rings: a terminal screen, where nothing is in progress.
    case none
    /// Static rings as decoration — a resting state that is nonetheless fine.
    case deco
    /// Breathing: waiting and watching. ±8% over `period`.
    case wait(period: Double = 1.4)
    /// Breathing, but brighter and faster: work is actually moving, not merely pending. Kept
    /// distinct from `wait` because `FirmwareView` uses the difference to separate "downloading"
    /// from "uploading to the car", where only the second one has a deadline the user feels.
    case active
    /// Expanding away from the centre: reaching out toward something else.
    case outward
    /// Converging on the centre: something is arriving here. The exact reverse of `outward`,
    /// which is what makes the two impossible to confuse at a glance.
    case inward
}

/// Rings behind the device. Drawn first, so an opaque body occludes them and they read as
/// passing *under* the object rather than over it.
struct DeviceRingsView: View {
    let mode: RingMode
    let palette: Palette
    /// Almost always the accent. `NoInternetView` wants them warm, and used to draw a whole
    /// second set of rings — and a second car — for want of this one parameter.
    var tint: Color? = nil
    private var ink: Color { tint ?? palette.accent }

    var body: some View {
        switch mode {
        case .none:
            EmptyView()
        case .deco:
            rings(scale: 1.0, opacity: [0.20, 0.11, 0.045])
        case .wait, .active:
            let period: Double = mode == .active ? 1.05 : waitPeriod
            let op: [Double] = mode == .active ? [0.62, 0.38, 0.20] : [0.42, 0.24, 0.11]
            TimelineView(.animation) { ctx in
                let t = ctx.date.timeIntervalSinceReferenceDate
                rings(scale: 1.0 + 0.08 * (0.5 + 0.5 * sin(t * 2 * .pi / period)), opacity: op)
            }
        case .outward, .inward:
            TimelineView(.animation) { ctx in
                let t = ctx.date.timeIntervalSinceReferenceDate
                ZStack {
                    ForEach(0..<2, id: \.self) { i in
                        let ph = ((t + Double(i) * 0.9) / 1.8).truncatingRemainder(dividingBy: 1)
                        // Inward runs the same phase backwards and brightens as it lands, so
                        // arrival is the loud moment; outward is brightest as it leaves.
                        Circle().stroke(ink, lineWidth: 2)
                            .frame(width: 60, height: 60)
                            .scaleEffect(mode == .inward ? 1.9 - ph : 0.9 + ph)
                            .opacity(0.6 * (mode == .inward ? ph : 1 - ph))
                    }
                }
            }
        }
    }

    private var waitPeriod: Double {
        if case .wait(let p) = mode { return p }
        return 1.4
    }

    private func rings(scale: Double, opacity: [Double]) -> some View {
        ZStack {
            ForEach(0..<3, id: \.self) { i in
                Circle().stroke(ink, lineWidth: 1.5)
                    .frame(width: DeviceArt.ringD[i], height: DeviceArt.ringD[i])
                    .opacity(opacity[i])
                    .scaleEffect(scale)
            }
        }
    }
}

/// The chip at a device's centre: what it is busy with, and whether that is going well.
///
/// Opaque by construction — `bg` under a tinted fill — for the same reason the bodies are: it
/// sits over rings that must not shine through it.
struct ChipBadge: View {
    let glyph: String
    let tint: Color
    let palette: Palette
    var haloRadius: CGFloat = 5

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 5).fill(palette.bg)
            RoundedRectangle(cornerRadius: 5).fill(tint.opacity(0.18))
            RoundedRectangle(cornerRadius: 5).stroke(tint, lineWidth: 1)
            Image(systemName: glyph).font(.system(size: 11, weight: .bold)).foregroundStyle(tint)
        }
        .frame(width: 20, height: 20)
        .shadow(color: tint.opacity(0.55), radius: haloRadius)
    }
}

/// The car, seen from above. 34×72 body, windshield near the front, four wheels drawn on top.
///
/// Opaque in three layers — `bg` base, `panel` fill, `metal` stroke — and the base is what makes
/// it a solid object: without it the rings behind would show through the body and the whole scene
/// would flatten into a diagram.
struct CarBody: View {
    let palette: Palette
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10).fill(palette.bg)
                .overlay(RoundedRectangle(cornerRadius: 10).fill(palette.panel))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(palette.metal, lineWidth: 1))
                .frame(width: 34, height: 72)
            RoundedRectangle(cornerRadius: 3).fill(palette.bg)
                .frame(width: 20, height: 8).offset(y: -25)
            ForEach(0..<4, id: \.self) { i in
                RoundedRectangle(cornerRadius: 3).fill(palette.metal)
                    .frame(width: 11, height: 15)
                    .offset(x: i % 2 == 0 ? -18.5 : 18.5, y: i < 2 ? -20.5 : 20.5)
            }
        }
    }
}

/// The USB adapter — deliberately the car's sibling, not a new kind of drawing.
///
/// Same three layers, same `metal`, same 20×20 chip well at its centre; a plug where the car has
/// wheels. That kinship is the point: the startup sequence hands the story from one device to the
/// other, and if the two were drawn in different languages every hand-off would read as a jump to
/// an unrelated screen. Landscape where the car is portrait, so which one is on screen is never
/// in doubt either.
struct AdapterBody: View {
    let palette: Palette
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 9).fill(palette.bg)
                .overlay(RoundedRectangle(cornerRadius: 9).fill(palette.panel))
                .overlay(RoundedRectangle(cornerRadius: 9).stroke(palette.metal, lineWidth: 1))
                .frame(width: 46, height: 34)
            // The plug, and the neck joining it to the shell.
            RoundedRectangle(cornerRadius: 3).fill(palette.metal)
                .frame(width: 8, height: 12).offset(x: -26)
            Rectangle().fill(palette.metal).frame(width: 6, height: 2).offset(x: -20)
            // Vents: the only ornament, and the thing that keeps the shell from reading as a
            // plain rounded rectangle at a glance.
            VStack(spacing: 3.2) {
                ForEach(0..<3, id: \.self) { _ in
                    RoundedRectangle(cornerRadius: 1.1).fill(palette.metal.opacity(0.5))
                        .frame(width: 8, height: 2.2)
                }
            }.offset(x: 15)
        }
    }
}

/// Rings behind, one opaque device on top, an optional chip at its centre.
///
/// `presence` is the second dial of the language: a device that has not been found yet is drawn
/// faint, and the step that finds it makes the *same* drawing solid rather than replacing it with
/// a different picture. That is what turns two consecutive screens into one movement forward.
struct DeviceScene<Device: View>: View {
    let palette: Palette
    var rings: RingMode = .wait()
    /// Ring colour, when the accent is not the right thing to say. Declared beside `rings`
    /// because it configures them, and because Swift's memberwise initialiser takes arguments
    /// in declaration order — a dial that reads as unrelated ends up in an odd place at every
    /// call site.
    var ringTint: Color? = nil
    var presence: Double = 1
    var chip: (glyph: String, tint: Color)? = nil
    /// The chip's halo. One screen wants it wider — a finished update is the one moment in the
    /// app worth a little celebration — so it is a dial rather than a constant.
    var chipHalo: CGFloat = 5
    /// A measured wait, drawn as an arc around the chip. Only screens that genuinely know how
    /// far along they are may pass this — a bar that invents its own progress is worse than no
    /// bar, because it teaches the user not to believe the next one.
    var progress: Double? = nil
    var offset: CGSize = .zero
    @ViewBuilder var device: () -> Device

    var body: some View {
        ZStack {
            DeviceRingsView(mode: rings, palette: palette, tint: ringTint).offset(offset)
            device().opacity(presence).offset(offset)
            if let chip {
                ChipBadge(glyph: chip.glyph, tint: chip.tint, palette: palette,
                          haloRadius: chipHalo).offset(offset)
                if let progress {
                    Circle()
                        .trim(from: 0, to: max(0, min(1, progress)))
                        .stroke(chip.tint, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                        .rotationEffect(.degrees(-90))
                        .frame(width: 34, height: 34)
                        .offset(offset)
                        .animation(.linear(duration: 0.2), value: progress)
                }
            }
        }
        .scaleEffect(DeviceArt.scale)
        .frame(width: DeviceArt.stage.width, height: DeviceArt.stage.height)
    }
}

/// The only screen with both devices in it: the adapter reaching the car.
///
/// Rings leave the adapter rather than the centre of the stage, and the car brightens as each
/// one arrives. That direction is the entire difference from the radar screen next door — there
/// a sweep turns about its own axis and searches, here motion travels from one object to
/// another — and it is legible before a word of the title is read.
struct LinkScene: View {
    let palette: Palette
    /// The same two devices with nothing travelling between them. Stillness is the message: this
    /// screen is a hold with a button, not a wait that resolves itself, and a wave leaving the
    /// adapter would promise otherwise.
    var failed: Bool = false
    private static let adapterX: CGFloat = -44
    private static let carX: CGFloat = 40

    var body: some View {
        if failed {
            ZStack {
                CarBody(palette: palette).opacity(0.3).offset(x: Self.carX)
                AdapterBody(palette: palette).offset(x: Self.adapterX)
                ChipBadge(glyph: "exclamationmark", tint: palette.warn, palette: palette)
                    .offset(x: Self.adapterX)
            }
            .scaleEffect(DeviceArt.scale)
            .frame(width: DeviceArt.stage.width, height: DeviceArt.stage.height)
        } else {
            live
        }
    }

    private var live: some View {
        TimelineView(.animation) { ctx in
            let t = ctx.date.timeIntervalSinceReferenceDate
            // Two waves, half a period apart, and the car's brightness read from the nearer of
            // them: arrival is computed from the wave's own radius, so the glow cannot drift out
            // of step with the thing that is supposed to be causing it.
            let phases = (0..<2).map { i in
                ((t + Double(i) * 0.9) / 1.8).truncatingRemainder(dividingBy: 1)
            }
            let reach = Self.carX - Self.adapterX - 20
            let arrived = phases
                .filter { 22 + $0 * 78 > reach }
                .map { 1 - $0 }
                .max() ?? 0

            ZStack {
                ForEach(Array(phases.enumerated()), id: \.offset) { _, ph in
                    EmitArc().stroke(palette.accent, lineWidth: 2)
                        .frame(width: 44 + ph * 156, height: 44 + ph * 156)
                        .opacity(0.6 * (1 - ph))
                        .offset(x: Self.adapterX)
                }
                CarBody(palette: palette)
                    .opacity(0.3 + 0.68 * arrived)
                    .offset(x: Self.carX)
                AdapterBody(palette: palette).offset(x: Self.adapterX)
            }
        }
        .scaleEffect(DeviceArt.scale)
        .frame(width: DeviceArt.stage.width, height: DeviceArt.stage.height)
    }
}

/// A wave front leaving one device for another: an arc facing the target, not a ring around the
/// emitter. The difference is the whole point of the screen it appears on — a full circle spends
/// half its ink behind the sender and says nothing about which way the signal is travelling.
struct EmitArc: Shape {
    var span: Angle = .degrees(126)
    func path(in rect: CGRect) -> Path {
        var p = Path()
        p.addArc(center: CGPoint(x: rect.midX, y: rect.midY),
                 radius: min(rect.width, rect.height) / 2,
                 startAngle: .degrees(-span.degrees / 2),
                 endAngle: .degrees(span.degrees / 2),
                 clockwise: false)
        return p
    }
}
