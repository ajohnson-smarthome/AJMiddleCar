import SwiftUI

struct SettingsView: View {
    let palette: Palette
    @ObservedObject var status: CarStatus
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ZStack {
                palette.bg.ignoresSafeArea()
                VStack(spacing: 0) {
                header
                List {
                    Section {
                        NavigationLink {
                            CarDimensionsView(palette: palette)
                        } label: {
                            Label(L.dimsTitle, systemImage: "ruler")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                        NavigationLink {
                            WheelParamsView(palette: palette)
                        } label: {
                            Label(L.wheelTitle, systemImage: "steeringwheel")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                        NavigationLink {
                            CalibrationView(palette: palette)
                        } label: {
                            Label(L.settingsCalibration, systemImage: "gearshape.2")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                    } header: {
                        sectionHeader(L.settingsGroupSetup)
                    }
                    Section {
                        NavigationLink {
                            RampView(palette: palette)
                        } label: {
                            Label(L.rampTitle, systemImage: "gauge.with.needle")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                        NavigationLink {
                            TrimView(palette: palette)
                        } label: {
                            Label(L.trimTitle, systemImage: "arrow.up.to.line")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                        NavigationLink {
                            RecoverView(palette: palette)
                        } label: {
                            Label(L.recoverTitle, systemImage: "arrow.uturn.backward")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                        NavigationLink {
                            TricksSettingsView(palette: palette)
                        } label: {
                            Label(L.tricksTitle, systemImage: "sparkles")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                    } header: {
                        sectionHeader(L.settingsGroupDriving)
                    }
                    Section {
                        NavigationLink {
                            FirmwareView(palette: palette, status: status)
                        } label: {
                            Label(L.settingsFirmware, systemImage: "arrow.down.circle")
                                .foregroundStyle(palette.text)
                        }
                        .listRowBackground(palette.panel)
                    } header: {
                        sectionHeader(L.settingsGroupSystem)
                    }
                }
                .scrollContentBackground(.hidden)
                // Reference info lives here, off the drive screen: uptime + firmware version.
                HStack(spacing: 12) {
                    HStack(spacing: 4) {
                        Image(systemName: "clock")
                        Text(status.uptimeS.map { L.uptime($0) } ?? "—")
                    }
                    HStack(spacing: 4) {
                        Image(systemName: "cpu")
                        Text(status.fw ?? "—")
                    }
                }
                .font(.system(size: 11))
                .foregroundStyle(palette.muted)
                .frame(maxWidth: .infinity)
                .frame(height: 52)   // centred vertically in the band between the list and the screen edge
                }
            }
            // Hide the system nav bar across the whole stack so it never toggles when a
            // SplitScreen child (which also hides it) is pushed/popped — otherwise the
            // reappearing bar re-insets this list and the content jumps on return.
            .toolbar(.hidden, for: .navigationBar)
        }
        .tint(palette.accent)
    }

    // Section header styled to the dark palette (the default gray header reads too light here).
    private func sectionHeader(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(palette.muted)
    }

    // Custom header matching the SplitScreen children (no system nav bar): title + Close.
    private var header: some View {
        HStack {
            Text(L.settingsTitle).font(.system(size: 17, weight: .semibold)).foregroundStyle(palette.text)
            Spacer()
            Button(L.close) { dismiss() }
                .font(.system(size: 16)).foregroundStyle(palette.accent)
        }
        .padding(.horizontal, 20).padding(.top, 14).padding(.bottom, 8)
    }
}

