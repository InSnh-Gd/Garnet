import SwiftUI
import AppKit
import UniformTypeIdentifiers
#if canImport(GarnetCore)
import GarnetCore
#endif

struct StatusTroubleshootView: View {
    @ObservedObject var model: GarnetAppModel
    @State private var manualProcName: String = ""
    @State private var showClearDenylistConfirm: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Runtime Status, App Denylist & Diagnostics")
                    .font(.title2).bold()
                Spacer()
                Button {
                    model.exportDiagnosticsOneClick()
                } label: {
                    Label("Export Diagnostics", systemImage: "square.and.arrow.up")
                }
                .disabled(model.isExportingDiag)
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
                            GridRow {
                                Text("AMFI Trust Mode:").bold()
                                Text(amfiStatusLabel(st))
                                    .foregroundColor(st.garnetAnchorLoaded ? .green : (st.amfiDisabledByBootArg ? .secondary : .orange))
                                Text("AMFIAnchor Status:").bold()
                                Text(st.amfiAnchorInstallStatus.map { "InstallStatus=\($0)" } ?? (st.amfiAnchorLoaded ? "Present" : "Not loaded"))
                                    .font(.system(.caption, design: .monospaced))
                            }
                        }
                        .font(.caption)
                        if let recNotice = st.recoveryCompleteNotice {
                            Label {
                                Text("Safe-Boot Recovery Uninstall Completed: \(recNotice.replacingOccurrences(of: "\n", with: " · "))")
                                    .font(.caption)
                            } icon: {
                                Image(systemName: "arrow.uturn.backward.circle.fill")
                            }
                            .foregroundColor(.blue)
                            .padding(.top, 4)
                        }
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
                    Text("Exports a .tar.gz archive with activation & translator logs, LaunchDaemon status, preflight JSON, driver/accelerator/display IORegistry dumps, system_profiler displays, kmutil showloaded output, and unified logs (username, home paths, serial number, and hardware UUID scrubbed).")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    HStack(spacing: 12) {
                        Button {
                            model.exportDiagnosticsOneClick()
                        } label: {
                            Label("One-Click Export to Desktop", systemImage: "square.and.arrow.up")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.isExportingDiag)

                        Button("Save As…") {
                            exportDiagnostics()
                        }
                        .disabled(model.isExportingDiag)

                        if let exportedURL = model.lastExportedDiagURL {
                            Button("Reveal in Finder") {
                                NSWorkspace.shared.activateFileViewerSelecting([exportedURL])
                            }
                        }

                        if model.isExportingDiag {
                            ProgressView()
                                .controlSize(.small)
                        }
                    }

                    if !model.diagExportMessage.isEmpty {
                        Text(model.diagExportMessage)
                            .font(.caption)
                            .foregroundColor(model.diagExportFailed ? .red : .green)
                            .textSelection(.enabled)
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

    private func amfiStatusLabel(_ st: RuntimeStatusReport) -> String {
        if st.garnetAnchorLoaded {
            return "AMFIAnchor (org.garnet.code-signing)"
        }
        if st.amfiDisabledByBootArg {
            return "Legacy (amfi=0x80)"
        }
        return st.amfiAnchorLoaded ? "AMFIAnchor (Anchor Missing)" : "Not Configured"
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
        panel.nameFieldStringValue = DiagnosticsManager.defaultDiagnosticsArchiveURL().lastPathComponent
        panel.allowedContentTypes = [UTType.gzip]
        if panel.runModal() == .OK, let outURL = panel.url {
            model.exportDiagnostics(to: outURL, revealInFinder: true)
        }
    }
}
