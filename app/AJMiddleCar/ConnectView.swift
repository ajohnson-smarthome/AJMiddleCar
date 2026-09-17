import SwiftUI
import UIKit
import Network

/// "The car is not answering" — and, since the link layer can now tell them apart, the three
/// other reasons a car is not answering: the dongle is unplugged, local-network access was
/// denied, or we are simply still looking. One radar for all of them is what this screen used to
/// be.
///
/// Extended for the dongle's own launch sequence (`AppFlow.dongleGate()`, `DongleLink`): before
/// the car is even reachable, the dongle itself can be being looked for, updating, rolled back,
/// answering badly, somebody else's adapter entirely, waiting to be told a network, or unable to
/// reach one it was already told. All of those render here, on the same radar, rather than as a
/// second screen family — from the seat this is watched from, "the car is not answering yet" and
/// "the adapter is not ready yet" are the same wait with a different reason underneath.
struct ConnectView: View {
    enum Situation: Equatable {
        case searching
        /// The newest release could not be established, so nothing may proceed. No button: the
        /// gate loop is still asking and clears this itself the moment the network returns.
        case releaseOffline
        /// A release exists and carries no image for `device` (or no build number). Carries the
        /// tag, because the only person who can act on this is the one who publishes releases,
        /// and the tag is what tells them which one to look at.
        case releaseMissing(tag: String, device: UpdateRules.Device)
        /// Step 3: asking GitHub for the newest release — the one tag both boards are compared
        /// against. Had no screen at all before — `dongleGate()` did this silently, so a launch
        /// that stopped here looked like a launch that had stopped for no reason.
        case releaseCheck
        /// One rung of the launch ladder, the board and the step. The single situation the runner
        /// emits.
        case stage(UpdateRules.Device, GateStep)
    }

    var situation: Situation = .searching
    /// `.stage` with a `.wrongDevice`, `.rolledBack` or `.joinFailed` step — the one button those
    /// carry. `wakePoll` / `recheckRollback` / `retryJoin` respectively; wired by `RootView`.
    var onRetry: (() -> Void)? = nil
    @Environment(\.colorScheme) private var colorScheme
    private var p: Palette { Theme.current(colorScheme) }

    var body: some View {
        SplitScreen(palette: p) {
            leftPanel
        } right: {
            rightPanel
        }
    }

    /// The radar sweep reads as "still looking, will resolve on its own" — true of every
    /// situation here except the ones that hand control to a button (`.stage`'s `.rolledBack`,
    /// `.wrongDevice`, `.joinFailed`): none of them retries itself, so none should look like it
    /// will — see `stageScene` for those. `.stage(_, .joining)` keeps the sweep: its retries are
    /// bounded but real, and while they are running the screen is telling the truth.
    @ViewBuilder private var leftPanel: some View {
        switch situation {
        case .releaseOffline:
            DeviceScene(palette: p, rings: .deco, ringTint: p.warn,
                        chip: (glyph: "wifi.exclamationmark", tint: p.warn)) { AdapterBody(palette: p) }
        case .releaseMissing:
            // A cross, not a warning triangle: there is nothing wrong with the adapter, there is
            // simply no image to compare it against.
            DeviceScene(palette: p, rings: .deco, ringTint: p.warn,
                        chip: (glyph: "xmark", tint: p.warn)) { AdapterBody(palette: p) }
        case .releaseCheck:
            DeviceScene(palette: p, rings: .inward,
                        chip: (glyph: "arrow.down", tint: p.accent)) { AdapterBody(palette: p) }
        case .stage(let d, let step):
            stageScene(d, step)
        // Everything left is a search of the air, which is the one thing the sweep means.
        case .searching:
            ConnectCarView(palette: p)
        }
    }

    /// `stageScene`'s device drawing, shared by every step that shows "found, in the frame" —
    /// hoisted out of `stageScene` because a local function marked `@ViewBuilder` nested inside
    /// another `@ViewBuilder` function does not compile on this toolchain ("closure containing a
    /// declaration cannot be used with result builder 'ViewBuilder'").
    @ViewBuilder private func stageBody(_ d: UpdateRules.Device) -> some View {
        if d == .car { CarBody(palette: p) } else { AdapterBody(palette: p) }
    }

    @ViewBuilder private func stageScene(_ d: UpdateRules.Device, _ step: GateStep) -> some View {
        switch step {
        case .seeking where d == .dongle, .absent:
            DeviceScene(palette: p, rings: .wait(), presence: 0.34) { AdapterBody(palette: p) }
        case .seeking, .checking:            // car .seeking and either board's .checking: found, being asked
            DeviceScene(palette: p, rings: .wait(), chip: (glyph: "cpu", tint: p.accent)) { stageBody(d) }
        case .fault:
            DeviceScene(palette: p, rings: .wait(), chip: (glyph: "exclamationmark", tint: p.warn)) { stageBody(d) }
        case .denied:
            DeviceScene(palette: p, rings: .deco, ringTint: p.warn, chip: (glyph: "lock", tint: p.warn)) { stageBody(d) }
        case .wrongDevice:
            DeviceScene(palette: p, rings: .deco, ringTint: p.warn, chip: (glyph: "questionmark", tint: p.warn)) { stageBody(d) }
        case .rolledBack:
            DeviceScene(palette: p, rings: .deco, ringTint: p.warn, chip: (glyph: "arrow.uturn.backward", tint: p.warn)) { stageBody(d) }
        case .appBehind:
            DeviceScene(palette: p, rings: .deco, ringTint: p.warn,
                        chip: (glyph: "exclamationmark.arrow.circlepath", tint: p.warn)) { stageBody(d) }
        case .updating:                      // never reached: FirmwareView renders .updating
            DeviceScene(palette: p, rings: .wait(), chip: (glyph: "arrow.down", tint: p.accent)) { stageBody(d) }
        case .sendingNetwork:
            DeviceScene(palette: p, rings: .inward, chip: (glyph: "wifi", tint: p.accent)) { AdapterBody(palette: p) }
        case .searching:
            ConnectCarView(palette: p)
        case .joining:
            LinkScene(palette: p)
        case .joinFailed:
            LinkScene(palette: p, failed: true)
        }
    }

    private var title: String {
        switch situation {
        case .searching: return L.connectTitle
        case .releaseOffline: return L.gateNoInternetTitle
        case .releaseMissing(_, let device): return L.gateNoReleaseTitle(device)
        case .releaseCheck: return L.gateReleaseCheckTitle
        case .stage(let d, let step): return L.stageTitle(step, d)
        }
    }

    private var message: String {
        switch situation {
        case .searching: return L.connectBody
        case .releaseOffline: return L.gateOfflineSub
        case .releaseMissing(let tag, let device): return L.gateNoReleaseSub(device, tag)
        case .releaseCheck: return L.gateReleaseCheckSub
        case .stage(let d, let step): return L.stageSub(step, d)
        }
    }

    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 9) {
            Text(title).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
            Text(message).font(.system(size: 13)).foregroundStyle(p.muted)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 260, alignment: .leading)
            actionButton
        }
    }

    /// The one button each situation can offer, if any. `.stage`'s `.denied` step opens Settings;
    /// its `.wrongDevice`, `.rolledBack` and `.joinFailed` steps are escapes from a gate that
    /// will not clear on its own — see `GateStep`'s doc comments for why each needs one at all.
    @ViewBuilder private var actionButton: some View {
        switch situation {
        case .stage(_, let step):
            switch step {
            case .denied:
                // `openSettingsURLString` opens *this app's* pane by definition — which is
                // precisely where the Local Network switch lives, so it is offered where it
                // helps and not where it would only look like a button.
                pillButton(L.openSettings, tint: p.accent) {
                    if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
                }
            case .wrongDevice, .rolledBack, .joinFailed:
                if let onRetry { pillButton(L.fwRetry, tint: p.warn, action: onRetry) }
            default:
                EmptyView()
            }
        case .searching, .releaseCheck, .releaseOffline, .releaseMissing:
            EmptyView()
        }
    }

    private func pillButton(_ text: String, tint: Color, filled: Bool = true,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(text)
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(tint)
                .padding(.horizontal, 16).padding(.vertical, 8)
                .background(RoundedRectangle(cornerRadius: 10).fill(filled ? tint.opacity(0.15) : Color.clear))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(filled ? tint.opacity(0.55) : p.line, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .padding(.top, 3)
    }
}

/// Dimmed car with a radar sweep behind it — "searching for the car".
///
/// Only the sweep is drawn here now. The car and the stage come from `DeviceArt`, so this
/// screen and the firmware screen build their car from one description instead of two copies of
/// the same numbers. The rings stay wider than the firmware screen's, and deliberately:
/// `DeviceArt.fieldD` is the air being searched, `ringD` is the device itself. Making them one
/// set was tried and the beam lost the room it needs.
///
/// The car is a sibling layer rather than something this file paints, which also gets the
/// occlusion for free: an opaque body over the beam is what makes the sweep read as passing
/// *under* the car instead of across it.
struct ConnectCarView: View {
    let palette: Palette
    /// One turn. The period is load-bearing for the whole screen's feel, and it is the number
    /// every other timing here is derived from.
    private static let period: Double = 2.6
    /// Where the returns sit: bearing in radians, range in points. Fixed rather than random —
    /// a radar whose echoes wander is a lava lamp, not an instrument.
    private static let blips: [(a: Double, r: Double)] =
        [(-0.6, 54), (2.1, 68), (3.6, 41), (5.2, 70)]

    var body: some View {
        ZStack {
            TimelineView(.animation) { tl in
                let t = tl.date.timeIntervalSinceReferenceDate
                ZStack {
                    Canvas { ctx, size in sweep(&ctx, size, time: t) }
                        .frame(width: DeviceArt.stage.width, height: DeviceArt.stage.height)
                    // The car is a return like any other: bright just after the beam has passed
                    // over it, fading until the next turn. A target lit constantly would say the
                    // search is already over, which is the opposite of what this screen means.
                    CarBody(palette: palette).opacity(carWash(time: t))
                }
            }
        }
        .scaleEffect(DeviceArt.scale)
        .frame(width: DeviceArt.stage.width, height: DeviceArt.stage.height)
    }

    /// How lit the car is: a function of the angle between the beam and the car's own bearing,
    /// so it is exactly as impossible for the two to drift apart as it is for the returns.
    private func carWash(time: Double) -> Double {
        let head = -(time * 360 / Self.period).truncatingRemainder(dividingBy: 360) * .pi / 180
        var delta = (head + .pi / 2).truncatingRemainder(dividingBy: 2 * .pi)
        if delta < 0 { delta += 2 * .pi }
        return 0.30 + 0.62 * exp(-2.2 * (delta / (2 * .pi) * Self.period))
    }

    private func sweep(_ ctx: inout GraphicsContext, _ size: CGSize, time: Double) {
        let c = CGPoint(x: size.width / 2, y: size.height / 2)
        let outer = DeviceArt.fieldD.last! / 2
        for d in DeviceArt.fieldD {
            let r = d / 2
            ctx.stroke(Path(ellipseIn: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)),
                       with: .color(palette.accent.opacity(0.16)), lineWidth: 1.5)
        }

        // The beam is a tail, not a wedge: brightest at the leading edge and decaying
        // exponentially over 120°, which is what a sweep actually looks like and what makes the
        // direction of travel readable without any other cue. A flat sector reads as a rotating
        // slice of pie — it was the single biggest thing making this screen look cheap.
        let head = -(time * 360 / Self.period).truncatingRemainder(dividingBy: 360)
        let arc = 1.0 / 3.0
        var stops: [Gradient.Stop] = (0...8).map { i in
            let f = Double(i) / 8
            return .init(color: palette.accent.opacity(0.32 * exp(-3.4 * f)), location: f * arc)
        }
        stops.append(.init(color: palette.accent.opacity(0), location: arc))
        stops.append(.init(color: palette.accent.opacity(0), location: 1))

        var beam = ctx
        beam.translateBy(x: c.x, y: c.y)
        beam.rotate(by: .degrees(head))
        beam.fill(Path(ellipseIn: CGRect(x: -outer, y: -outer, width: 2 * outer, height: 2 * outer)),
                  with: .conicGradient(Gradient(stops: stops), center: .zero, angle: .degrees(0)))

        // Returns: brightest just after the beam has crossed their bearing, then fading over the
        // rest of the turn. Brightness comes from the angle between the beam and the target, so
        // the flare and the sweep cannot drift out of step the way two timers would.
        let headRad = head * .pi / 180
        for b in Self.blips {
            var delta = (headRad - b.a).truncatingRemainder(dividingBy: 2 * .pi)
            if delta < 0 { delta += 2 * .pi }
            let lum = exp(-1.5 * (delta / (2 * .pi) * Self.period))
            guard lum > 0.02 else { continue }
            let p = CGPoint(x: c.x + cos(b.a) * b.r, y: c.y + sin(b.a) * b.r)
            ctx.fill(Path(ellipseIn: CGRect(x: p.x - 2.2, y: p.y - 2.2, width: 4.4, height: 4.4)),
                     with: .color(palette.accent.opacity(0.85 * lum)))
        }
    }
}
