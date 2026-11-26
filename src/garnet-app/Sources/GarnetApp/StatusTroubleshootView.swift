import SwiftUI
import AppKit
import UniformTypeIdentifiers
#if canImport(GarnetCore)
import GarnetCore
#endif

struct StatusTroubleshootView: View {
    @ObservedObject var model: GarnetAppModel
    @State private var manualProcName: String = ""
    @State private var diagExportMessage: String = ""
    @State private var diagExportFailed: Bool = false
    @State private var showClearDenylistConfirm: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Runtime Status, App Denylist & Diagnostics")
                    .font(.title2).bold()
                Spacer()
                Button("Refresh Status") { model.refreshAll() }
                    .disabled(model.isBusy)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
            Divider()

            Form {
                Section("Driver & Display Status") {
                    if let st = model.runtimeStatus {
                        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
                            GridRow {
                                Text("Garnet.kext:").bold()
                                Text(st.driverLoaded ? "Loaded" : "Not loaded").foregroundColor(st.driverLoaded ? .green : .secondary)
                                Text("GarnetAccel.kext:").bold()
                                Text(st.accelLoaded ? "Loaded" : "Not loaded").foregroundColor(st.accelLoaded ? .green : .secondary)
                            }
                            GridRow {
                                Text("GarnetPCIBarCompat:").bold()
                                Text(st.pciBarCompatLoaded ? "Loaded (\(st.iopciCompatMatchMode ?? "?"))" : "Not loaded")
                                Text("IOPCIFamily UUID:").bold()
                                Text(st.iopciCompatUUID ?? "n/a").font(.system(.caption, design: .monospaced))
                            }
                            GridRow {
                                Text("BAR0 Aperture:").bold()
                                Text(st.bar0CurrentMB.map { "\($0) MB" } ?? "unknown")
                                Text("Display:").bold()
                                Text(st.displayPrimaryStatus ?? "unknown")
                            }
                            GridRow {
                                Text("Installed Payload:").bold()
                                Text(st.installedVersion ?? "none")
                                Text("Vulkan ICD:").bold()
                                Text(st.vulkanInstalled ? "Installed (Optional)" : "Disabled (Default)")
                            }
                            GridRow {
                                Text("SMU Telemetry:").bold()
                                Text(smuTelemetryLabel(st))
                                    .foregroundColor(smuTelemetryColor(st))
                                Text("SMU Driver IF:").bold()
                                Text(st.smuDriverIfVersion ?? "n/a")
                                    .font(.system(.caption, design: .monospaced))
                            }
                        }
                        .font(.caption)
                        if st.smuTelemetryState == .unverified {
                            Label {
                                Text(smuUnverifiedWarning(st))
                                    .font(.caption)
                            } icon: {
                                Image(systemName: "exclamationmark.triangle.fill")
                            }
                            .foregroundColor(st.smuMetricsPlausible == false ? .red : .orange)
                            .padding(.top, 4)
                        }
                    } else {
                        Text("Collecting runtime status...").font(.caption).foregroundColor(.secondary)
                    }
                }

                Section("Metal Application Denylist") {
                    Text("Desktop Metal apps use Garnet GPU acceleration by default (garnet-apps-all=1) with 3-strike timeout fallback. Add misbehaving apps below; WindowServer and GARNET_ALLOW=1 root tools are always immune. Stored in \(GarnetPaths.denylistPath).")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    FileDropRow(
                        systemImage: "app.dashed",
                        message: "Drop an .app bundle here or choose one to extract its bundle ID and executable",
                        buttonTitle: "Choose .app…",
                        disabled: model.isBusy,
                        onPick: pickAppToDenylist,
                        onDropURL: { model.addDenylistApp(at: $0) }
                    )

                    HStack {
                        TextField("Process name (e.g. AdobeIllustrator)", text: $manualProcName)
                            .textFieldStyle(.roundedBorder)
                        Button("Add Process…") {
                            let v = manualProcName
                            manualProcName = ""
                            model.addDenylistProc(v)
                        }
                        .disabled(model.isBusy || manualProcName.trimmingCharacters(in: .whitespaces).isEmpty)

                        Button("Clear All…", role: .destructive) {
                            showClearDenylistConfirm = true
                        }
                        .disabled(model.isBusy || model.denylistEntries.isEmpty)
                    }

                    if model.denylistEntries.isEmpty {
                        Text("No custom denylist entries configured.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        ForEach(model.denylistEntries) { entry in
                            HStack {
                                Text(entry.rawLine)
                                    .font(.system(.caption, design: .monospaced))
                                Spacer()
                                Button("Remove…") {
                                    model.removeDenylistEntry(entry)
                                }
                                .buttonStyle(.borderless)
                                .disabled(model.isBusy)
                            }
                        }
                    }
                }

                Section("Troubleshooting Diagnostics") {
                    Text("Exports a .tar.gz archive with preflight JSON, driver/PCI ioregistry dumps, kmutil showloaded output, and recent Garnet kernel logs (username, home paths, serial number, and IOPlatformUUID scrubbed).")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    HStack {
                        Button {
                            exportDiagnostics()
                        } label: {
                            Label("Export Diagnostics Bundle…", systemImage: "square.and.arrow.up")
                        }
                        if !diagExportMessage.isEmpty {
                            Text(diagExportMessage).font(.caption).foregroundColor(diagExportFailed ? .red : .green)
                        }
                    }
                }
            }
            .formStyle(.grouped)
            .confirmationDialog(
                "Remove all \(model.denylistEntries.count) denylist \(model.denylistEntries.count == 1 ? "entry" : "entries")?",
                isPresented: $showClearDenylistConfirm
            ) {
                Button("Clear All", role: .destructive) {
                    model.clearDenylist()
                }
                Button("Cancel", role: .cancel) {}
            }
        }
    }

    private func smuTelemetryLabel(_ st: RuntimeStatusReport) -> String {
        switch st.smuTelemetryState {
        case .verified:
            return "Verified"
        case .unverified:
            return st.smuMetricsPlausible == false ? "UNVERIFIED (Range Check Failed)" : "UNVERIFIED"
        case .unavailable:
            return "Unavailable"
        }
    }

    private func smuTelemetryColor(_ st: RuntimeStatusReport) -> Color {
        switch st.smuTelemetryState {
        case .verified: return .green
        case .unverified: return st.smuMetricsPlausible == false ? .red : .orange
        case .unavailable: return .secondary
        }
    }

    private func smuUnverifiedWarning(_ st: RuntimeStatusReport) -> String {
        let ver = st.smuDriverIfVersion ?? "unknown"
        let rangeNote = st.smuMetricsPlausible == false ? " Range check FAILED." : ""
        return "SMU PMFW interface \(ver) is outside the verified telemetry layout (0x2e–0x33).\(rangeNote) Reported clocks and sensors may be inaccurate."
    }

    private func pickAppToDenylist() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [UTType.applicationBundle]
        panel.prompt = "Add to Denylist"
        if panel.runModal() == .OK, let u = panel.url {
            model.addDenylistApp(at: u)
        }
    }

    private func exportDiagnostics() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "garnet-diagnostics.tar.gz"
        panel.allowedContentTypes = [UTType.gzip]
        if panel.runModal() == .OK, let outURL = panel.url {
            do {
                let saved = try DiagnosticsManager.exportSanitizedDiagnostics(
                    toArchive: outURL,
                    env: HostEnvironment(),
                    payloadURL: model.payloadURL
                )
                diagExportFailed = false
                diagExportMessage = "Saved \(saved.lastPathComponent)"
            } catch {
                diagExportFailed = true
                diagExportMessage = "Failed: \(error)"
            }
        }
    }
}
