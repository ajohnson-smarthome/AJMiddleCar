import SwiftUI

/// Phases of a firmware update, shared by both devices' screens and their images.
enum FwPhase { case checking, upToDate, available, downloading, downloaded, uploading, rebooting, flashed, done, failed }

/// The device being updated, drawn top-down with a chip carrying the phase and OTA rings
/// around it.
///
/// One view for both devices, because the update flow is one flow: the car and the adapter run
/// the same ten phases through the same screen, and the only thing that differs is which object
/// is under the chip. Everything else — the rings, the glyphs, the tints — comes from
/// `DeviceArt`, and is chosen by the phase alone.
struct FirmwareDeviceView: View {
    var device: UpdateRules.Device = .car
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
            if device == .car {
                CarBody(palette: palette)
            } else {
                AdapterBody(palette: palette)
            }
        }
        .opacity(phase == .rebooting ? 0.85 : 1)
    }
}
