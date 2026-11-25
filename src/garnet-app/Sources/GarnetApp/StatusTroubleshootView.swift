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
    @State private var appDropTargeted: Bool = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text("Runtime Status, App Denylist & Diagnostics")
                        .font(.title2).bold()
                    Spacer()
                    Button("Refresh Status") { model.refreshAll() }
                        .disabled(model.isBusy)
                }

                GroupBox("1. Live Driver & Display Status") {
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
                        }
                        .font(.caption)
                        .padding(6)
                    } else {
                        Text("Collecting runtime status...").font(.caption).foregroundColor(.secondary)
                    }
                }

                GroupBox("2. Metal Application Denylist (\(GarnetPaths.denylistPath))") {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("By default (`garnet-apps-all=1`), desktop Metal apps use Garnet GPU acceleration with 3-strike timeout fallback. Add misbehaving apps below; WindowServer and `GARNET_ALLOW=1` root tools are always immune.")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        ZStack {
                            RoundedRectangle(cornerRadius: 8)
                                .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6]))
                                .foregroundColor(appDropTargeted ? .accentColor : .secondary.opacity(0.5))
                                .frame(height: 54)
                            HStack {
                                Image(systemName: "app.dashed")
                                Text("Drop an .app bundle here or choose one to extract CFBundleIdentifier + executable name")
                                    .font(.caption)
                                Button("Choose .app…") {
                                    pickAppToDenylist()
                                }
                                .disabled(model.isBusy)
                            }
                        }
                        .onDrop(of: [.fileURL], isTargeted: $appDropTargeted) { providers in
                            handleAppDrop(providers: providers)
                        }

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
                                model.clearDenylist()
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
                                .padding(.vertical, 2)
                            }
                        }
                    }
                    .padding(6)
                }

                GroupBox("3. Export Sanitized Troubleshooting Diagnostics") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Creates a `.tar.gz` containing preflight JSON, driver/PCI ioregistry dumps, `kmutil showloaded`, and recent Garnet kernel logs with username, home paths, serial number, and IOPlatformUUID scrubbed.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Button {
                                exportDiagnostics()
                            } label: {
                                Label("Export Diagnostics Bundle…", systemImage: "square.and.arrow.up")
                            }
                            if !diagExportMessage.isEmpty {
                                Text(diagExportMessage).font(.caption).foregroundColor(.green)
                            }
                        }
                    }
                    .padding(6)
                }
            }
            .padding()
        }
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

    private func handleAppDrop(providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first(where: { $0.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) }) else {
            return false
        }
        provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
            guard let data = item as? Data,
                  let url = URL(dataRepresentation: data, relativeTo: nil) else { return }
            Task { @MainActor in
                model.addDenylistApp(at: url)
            }
        }
        return true
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
                diagExportMessage = "Saved \(saved.lastPathComponent)"
            } catch {
                diagExportMessage = "Failed: \(error)"
            }
        }
    }
}
