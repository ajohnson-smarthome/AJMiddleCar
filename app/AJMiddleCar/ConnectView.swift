import SwiftUI
import UIKit
import Network

/// "The car is not answering" — and, since the link layer can now tell them apart, the three
/// other reasons a car is not answering: the dongle is unplugged, local-network access was
/// denied, or we are simply still looking. One radar for all of them is what this screen used to
/// be.
///
/// Extended for the dongle's own launch sequence (`AppFlow.dongleGate()`, `DongleLink`): before
/// the car is even reachable, the dongle itself can be updating, waiting to be told a network,
/// or unable to reach one it was already told. Those four cases render here too, on the same
/// radar, rather than as a second screen family — from the seat this is watched from, "the car
/// is not answering yet" and "the adapter is not ready yet" are the same wait with a different
/// reason underneath.
struct ConnectView: View {
    enum Situation: Equatable {
        case searching
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
        default:
            ConnectCarView(palette: p)
        }
    }

    private var title: String {
        switch situation {
        case .searching: return L.connectTitle
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
        case .searching, .noDongle, .dongleUpdating, .dongleConfiguring,
             .dongleFault, .wrongDongle:
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
struct ConnectCarView: View {
    let palette: Palette
    private var metal: Color { palette.metal }
    private let carW: CGFloat = 34
    private let carLen: CGFloat = 72
    private let wheelW: CGFloat = 11
    private let wheelH: CGFloat = 15

    var body: some View {
        TimelineView(.animation) { tl in
            Canvas { ctx, size in
                render(&ctx, size, time: tl.date.timeIntervalSinceReferenceDate)
            }
        }
        .frame(width: 160, height: 210)
        .scaleEffect(1.6)
    }

    private func render(_ ctx: inout GraphicsContext, _ size: CGSize, time: Double) {
        let center = CGPoint(x: size.width / 2, y: size.height / 2)
        // radar grid: three faint rings
        for r in [46.0, 60.0, 74.0] {
            let rect = CGRect(x: center.x - r, y: center.y - r, width: 2 * r, height: 2 * r)
            ctx.stroke(Path(ellipseIn: rect), with: .color(palette.accent.opacity(0.16)), lineWidth: 1.5)
        }
        // rotating beam: 70° sector with a fading conic tail, full turn ≈ 2.6 s,
        // sweeping counter-clockwise (user preference)
        var beam = ctx
        beam.translateBy(x: center.x, y: center.y)
        beam.rotate(by: .degrees(-(time * 360 / 2.6).truncatingRemainder(dividingBy: 360)))
        var sector = Path()
        sector.move(to: .zero)
        sector.addArc(center: .zero, radius: 74,
                      startAngle: .degrees(-70), endAngle: .degrees(0), clockwise: false)
        sector.closeSubpath()
        // CCW sweep → leading (bright) edge at -70°, tail fades toward 0°
        beam.fill(sector, with: .conicGradient(
            Gradient(colors: [palette.accent.opacity(0.35), palette.accent.opacity(0.0)]),
            center: .zero, angle: .degrees(-70)))
        // dimmed car on top — opaque base so the beam reads as passing UNDER the car
        drawCar(&ctx, center: center)
    }

    private func drawCar(_ ctx: inout GraphicsContext, center: CGPoint) {
        // Opaque bg fills occlude the beam; the "dimmed" look comes from muted colour mixes,
        // not from layer transparency (which would let the beam shine through the body).
        let body = CGRect(x: center.x - carW / 2, y: center.y - carLen / 2, width: carW, height: carLen)
        let bp = Path(roundedRect: body, cornerRadius: 11)
        ctx.fill(bp, with: .color(palette.bg))
        ctx.fill(bp, with: .color(palette.panel.opacity(0.6)))
        ctx.stroke(bp, with: .color(metal.opacity(0.6)), lineWidth: 1)
        let wind = CGRect(x: center.x - 11, y: body.minY + 7, width: 22, height: 9)
        ctx.fill(Path(roundedRect: wind, cornerRadius: 3), with: .color(palette.bg.opacity(0.85)))
        let wx = carW / 2 + 1
        let wy = carLen / 2 - 16
        for dx in [-wx, wx] {                       // wheels on top of the body, as in the reference
            for dy in [-wy, wy] {
                let r = CGRect(x: center.x + dx - wheelW / 2, y: center.y + dy - wheelH / 2,
                               width: wheelW, height: wheelH)
                let wp = Path(roundedRect: r, cornerRadius: 3)
                ctx.fill(wp, with: .color(palette.bg))
                ctx.fill(wp, with: .color(metal.opacity(0.6)))
            }
        }
    }
}
