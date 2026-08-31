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
        /// Step 1 of the startup ladder: nothing has answered at the adapter's address yet.
        /// Shows the adapter faint — the same drawing the next step makes solid, which is what
        /// turns the pair into one movement forward rather than two unrelated pictures.
        case findingAdapter
        /// Step 3: the adapter is ours and healthy; asking GitHub whether it is current. Had no
        /// screen at all before — `dongleGate()` did this silently, so a launch that stopped
        /// here looked like a launch that had stopped for no reason.
        case adapterUpdateCheck
        /// Step 4: the radio is scanning and has not seen the car's network yet. Distinct from
        /// `.dongleConfiguring`, which is the association that follows — see `WifiState`.
        case findingCar
        /// Step 6: the car is reachable; asking GitHub about the car's own firmware.
        case carUpdateCheck
        /// The first frame of a launch: the dongle has been asked and has not answered yet.
        /// Its own line, because `.searching`'s says the CAR is not answering — an assertion
        /// about a device nothing has spoken to yet, made before the adapter in front of it
        /// has even been found.
        case checkingDongle
        case noDongle(NWPath.UnsatisfiedReason)
        case localNetworkDenied
        /// Something answered at the dongle's address and it was not usable — an HTTP error, a
        /// truncated stream, a body that would not decode (`DongleStep.faulty`). The radar
        /// stays: the flow keeps polling and one bad answer is often a dongle mid-boot.
        case dongleFault
        /// A USB-Ethernet adapter answered and it is not ours — `status.device` disagrees with
        /// `DongleContract.device`. The dongle's analogue of `WrongCarView`, and like it, it
        /// names what answered rather than leaving the user to guess.
        case wrongDongle(String)
        /// The dongle's own firmware is behind the latest release; it is downloading and
        /// flashing it before anything else in the sequence touches the car.
        case dongleUpdating
        /// `performDongleUpdate()` failed `maxDongleUpdateAttempts` times running — no usable
        /// internet or cache, or the upload itself kept failing. `onRetryDongleUpdate` gives a
        /// way to ask for a fresh budget rather than watching it loop forever.
        case dongleUpdateFailed
        /// The dongle is current and pointed at the car's own network. Either its credentials
        /// are being sent for the first time (including a re-point, if it was pointed at some
        /// other network), or the radio is working through its own join budget — both read as
        /// "connecting" from here; see `DongleLink.DongleStep.sendCredentials`/`.waiting`.
        case dongleConfiguring
        /// The dongle will not reach the car on its own: the join budget ran out, or its state
        /// machine never left `idle`. `AppFlow` asks the radio to try again a bounded number of
        /// times and then holds here, so this screen carries `onRetryJoin` — without it, the
        /// hold would be a dead end and the retries would have to run forever to avoid one.
        case dongleJoinFailed
        /// The dongle's bootloader reverted its last update. Standing until the user answers —
        /// the flag itself does not clear on its own, so without an answer this would be a
        /// locked room rather than a message. Two answers: `onRecheckRollback` asks whether a
        /// newer release exists yet (the only path back to an update, since the app is the
        /// dongle's only OTA path), `onSkipRollback` drives on what is running.
        case dongleRolledBack
    }

    var situation: Situation = .searching
    /// `.dongleRolledBack` only. The car's own forced-update gate keeps an identical skip
    /// (`FirmwareView.skipButton`) for the identical reason: a gate that failed must not be a
    /// dead end.
    var onSkipRollback: (() -> Void)? = nil
    /// `.dongleRolledBack` only, beside the skip — exactly as `FirmwareView`'s rolled-back car
    /// screen offers `fw.retry` beside its own. Without it the skip is the only exit, and it is
    /// a one-way one: the dongle's rollback flag only clears when a later OTA succeeds, and
    /// this app is the only thing that can perform one.
    var onRecheckRollback: (() -> Void)? = nil
    /// `.dongleUpdateFailed` only.
    var onRetryDongleUpdate: (() -> Void)? = nil
    /// `.dongleJoinFailed` only. The same offer as `onRetryDongleUpdate`, for the other bounded
    /// wait — asking again now, and with a fresh budget.
    var onRetryJoin: (() -> Void)? = nil
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
    /// situation here except the ones that hand control to a button (`.dongleRolledBack`,
    /// `.dongleUpdateFailed`, `.wrongDongle`): none of them retries itself, so none should look
    /// like it will. They borrow `WrongCarView`'s static failed-car image instead, the same way
    /// `WrongCarView` does. `.dongleJoinFailed` keeps the sweep: its retries are bounded but
    /// real, and while they are running the screen is telling the truth.
    @ViewBuilder private var leftPanel: some View {
        switch situation {
        // `.wrongDongle` joins them: nothing about it resolves by waiting either — the fix is a
        // different cable, and the sweep would promise otherwise.
        case .dongleRolledBack, .dongleUpdateFailed, .wrongDongle:
            FirmwareCarView(phase: .failed, palette: p)
        // The adapter's own three steps. Only two dials move across them: the body goes from
        // faint to solid when it is found, and the rings turn inward when something is arriving.
        case .findingAdapter, .noDongle:
            DeviceScene(palette: p, rings: .wait(), presence: 0.34) { AdapterBody(palette: p) }
        case .checkingDongle:
            DeviceScene(palette: p, rings: .wait(),
                        chip: (glyph: "cpu", tint: p.accent)) { AdapterBody(palette: p) }
        case .dongleFault:
            // Answering, but wrongly: the adapter is present, so it is drawn present, and the
            // chip carries the fault. The radar used to sit here and promised a search nobody
            // was performing.
            DeviceScene(palette: p, rings: .wait(),
                        chip: (glyph: "exclamationmark", tint: p.warn)) { AdapterBody(palette: p) }
        case .adapterUpdateCheck, .dongleUpdating:
            DeviceScene(palette: p, rings: .inward,
                        chip: (glyph: "arrow.down", tint: p.accent)) { AdapterBody(palette: p) }
        case .carUpdateCheck:
            DeviceScene(palette: p, rings: .inward,
                        chip: (glyph: "arrow.down", tint: p.accent)) { CarBody(palette: p) }
        case .dongleConfiguring:
            LinkScene(palette: p)
        // Everything left is a search of the air, which is the one thing the sweep means.
        case .searching, .findingCar, .localNetworkDenied, .dongleJoinFailed:
            ConnectCarView(palette: p)
        }
    }

    private var title: String {
        switch situation {
        case .searching: return L.connectTitle
        case .findingAdapter: return L.dongleFindingTitle
        case .adapterUpdateCheck: return L.dongleUpdCheckTitle
        case .findingCar: return L.carFindingTitle
        case .carUpdateCheck: return L.carUpdCheckTitle
        case .checkingDongle: return L.dongleCheckingTitle
        case .noDongle: return L.linkNoDongleTitle
        case .localNetworkDenied: return L.linkDeniedTitle
        case .dongleFault: return L.dongleFaultTitle
        case .wrongDongle: return L.dongleWrongTitle
        case .dongleUpdating: return L.dongleUpdatingTitle
        case .dongleUpdateFailed: return L.fwFailTitle
        case .dongleConfiguring: return L.dongleConfiguringTitle
        case .dongleJoinFailed: return L.dongleJoinFailedTitle
        case .dongleRolledBack: return L.dongleRolledBackTitle
        }
    }

    private var message: String {
        switch situation {
        case .searching: return L.connectBody
        case .findingAdapter: return L.dongleFindingSub
        case .adapterUpdateCheck: return L.dongleUpdCheckSub
        case .findingCar: return L.carFindingSub
        case .carUpdateCheck: return L.carUpdCheckSub
        case .checkingDongle: return L.dongleCheckingSub
        case .noDongle: return L.linkNoDongleSub
        case .localNetworkDenied: return L.linkDeniedSub
        case .dongleFault: return L.dongleFaultSub
        case .wrongDongle(let device): return L.dongleWrongSub(device)
        case .dongleUpdating: return L.dongleUpdatingSub
        case .dongleUpdateFailed: return L.dongleUpdateFailedSub
        case .dongleConfiguring: return L.dongleConfiguringSub
        case .dongleJoinFailed: return L.dongleJoinFailedSub
        case .dongleRolledBack: return L.dongleRolledBackSub
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

    /// The one button each situation can offer, if any. `.localNetworkDenied` opens Settings;
    /// `.dongleRolledBack` and `.dongleUpdateFailed` are the two dongle-side escapes from a gate
    /// that will not clear on its own — see their `Situation` doc comments for why each needs
    /// one at all.
    @ViewBuilder private var actionButton: some View {
        switch situation {
        case .localNetworkDenied:
            // `openSettingsURLString` opens *this app's* pane by definition — which is precisely
            // where the Local Network switch lives, so it is offered where it helps and not
            // where it would only look like a button.
            pillButton(L.openSettings, tint: p.accent) {
                if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
            }
        case .dongleRolledBack:
            HStack(spacing: 10) {
                // The offer, prominent, like `FirmwareView`'s own `fw.retry` on the same
                // situation: ask whether a newer release exists yet.
                if let onRecheckRollback {
                    pillButton(L.fwRetry, tint: p.warn, action: onRecheckRollback)
                }
                // Ghost styling, matching `FirmwareView.skipButton`'s own choice for the
                // identical situation: continuing on the reverted firmware is the fallback.
                if let onSkipRollback {
                    pillButton(L.fwSkip, tint: p.muted, filled: false, action: onSkipRollback)
                }
            }
        case .dongleUpdateFailed:
            if let onRetryDongleUpdate {
                pillButton(L.fwRetry, tint: p.warn, action: onRetryDongleUpdate)
            }
        case .dongleJoinFailed:
            if let onRetryJoin {
                pillButton(L.fwRetry, tint: p.warn, action: onRetryJoin)
            }
        case .searching, .checkingDongle, .noDongle, .dongleUpdating, .dongleConfiguring,
             .dongleFault, .wrongDongle,
             .findingAdapter, .adapterUpdateCheck, .findingCar, .carUpdateCheck:
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
