import SwiftUI

/// Phases of the firmware-update screen, shared by FirmwareView and its car image.
enum FwPhase { case checking, upToDate, available, downloading, downloaded, uploading, rebooting, flashed, done, failed }

/// The firmware screen's car: top-down body, a chip carrying the phase, OTA rings around it.
///
/// Every part of it now comes from `DeviceArt`, which is where the car, the rings and the chip
/// live for the whole app. This view is what is left once they do: a mapping from `FwPhase` to
/// the three dials of that language — which rings, which chip glyph, which tint.
struct FirmwareCarView: View {
    let phase: FwPhase
    let palette: Palette

    private var rings: RingMode {
        switch phase {
        case .checking, .downloading, .downloaded: return .wait()
        case .uploading: return .active
        case .rebooting: return .outward
        case .upToDate, .flashed: return .deco
        case .available, .done, .failed: return .none
        }
    }
    private var chipIcon: String {
        switch phase {
        case .upToDate, .done, .flashed: return "checkmark"
        case .failed: return "exclamationmark"
        default: return "cpu"
        }
    }
    private var chipColor: Color { phase == .failed ? palette.warn : palette.accent }

    var body: some View {
        DeviceScene(palette: palette, rings: rings,
                    chip: (glyph: chipIcon, tint: chipColor),
                    chipHalo: phase == .done ? 8 : 5) {
            CarBody(palette: palette)
        }
        .opacity(phase == .rebooting ? 0.85 : 1)
    }
}
