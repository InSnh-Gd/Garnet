import SwiftUI
import AppKit
import UniformTypeIdentifiers
#if canImport(GarnetCore)
import GarnetCore
#endif

struct OpenCoreAssistantView: View {
    @ObservedObject var model: GarnetAppModel
    @State private var exportStatus: String = ""
    @State private var copiedBanner: String?
    @State private var dropTargeted: Bool = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("OpenCore Configuration Assistant")
                    .font(.title2).bold()
                Text("Garnet never mounts or writes your EFI System Partition automatically. Export GarnetPCIBarCompat.kext manually and verify any OpenCore config.plist copy read-only below.")
                    .font(.caption)
                    .foregroundColor(.secondary)

                GroupBox("Where Each Kext Belongs (Strict Separation)") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
                        GridRow {
                            Text("Component").bold()
                            Text("Install Location").bold()
                            Text("Role").bold()
                        }
                        Divider()
                        GridRow {
                            Text("GarnetPCIBarCompat.kext")
                            Text("OpenCore EFI/OC/Kexts/ (16G mode only)").foregroundColor(.blue)
                            Text("Early IOPCIFamily 16 GiB BAR hook (NOT needed in 256M mode)")
                        }
                        GridRow {
                            Text("Garnet.kext")
                            Text("/Library/Extensions/ (AuxKC)").foregroundColor(.green)
                            Text("Never in EFI/OC/Kexts/ (links SystemKC IOGraphicsFamily)")
                        }
                        GridRow {
                            Text("GarnetAccel.kext")
                            Text("/Library/Extensions/ (AuxKC)").foregroundColor(.green)
                            Text("Never in EFI/OC/Kexts/ (matches host IOAcceleratorFamily2)")
                        }
                    }
                    .font(.caption)
                    .padding(6)
                }

                GroupBox("1. Export GarnetPCIBarCompat.kext for OpenCore EFI/OC/Kexts/") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Required for 16 GiB Full-BAR mode (load after Lilu/VirtualSMC, before Garnet). Not used for 256 MiB Small-BAR fallback.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Button {
                                chooseAndExportPCIBarCompat()
                            } label: {
                                Label("Export GarnetPCIBarCompat.kext…", systemImage: "folder.badge.plus")
                            }
                            .disabled(model.payloadURL == nil)

                            if !exportStatus.isEmpty {
                                Text(exportStatus)
                                    .font(.caption)
                                    .foregroundColor(.green)
                            }
                        }
                    }
                    .padding(6)
                }

                GroupBox("2. config.plist Checklist & Snippets") {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Picker("Target Mode", selection: $model.selectedOCMode) {
                                Text("16 GiB Full-BAR (Default)").tag(OpenCoreMode.mode16g)
                                Text("256 MiB Small-BAR Fallback").tag(OpenCoreMode.modeSmall)
                            }
                            .pickerStyle(.segmented)
                            .frame(maxWidth: 420)

                            Spacer()

                            Button("Copy Snippet") {
                                NSPasteboard.general.clearContents()
                                NSPasteboard.general.setString(OpenCoreValidator.sampleConfigSnippet(mode: model.selectedOCMode), forType: .string)
                                copiedBanner = "Copied!"
                            }
                            if let copiedBanner {
                                Text(copiedBanner).font(.caption).foregroundColor(.green)
                            }
                        }

                        ScrollView {
                            Text(OpenCoreValidator.sampleConfigSnippet(mode: model.selectedOCMode))
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .frame(height: 155)
                        .padding(6)
                        .background(Color(nsColor: .textBackgroundColor))
                        .cornerRadius(6)

                        Text(OpenCoreValidator.emergencyKillSwitchesText())
                            .font(.system(.caption2, design: .monospaced))
                            .textSelection(.enabled)
                            .foregroundColor(.secondary)
                    }
                    .padding(6)
                }

                GroupBox("3. Read-Only config.plist Inspector (Drag & Drop or Choose File)") {
                    VStack(alignment: .leading, spacing: 10) {
                        ZStack {
                            RoundedRectangle(cornerRadius: 8)
                                .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6]))
                                .foregroundColor(dropTargeted ? .accentColor : .secondary.opacity(0.5))
                                .frame(height: 68)
                            HStack(spacing: 12) {
                                Image(systemName: "doc.badge.gearshape")
                                    .font(.title2)
                                Text("Drop an OpenCore config.plist here (strictly read-only — SHA-256 unchanged)")
                                    .font(.caption)
                                Button("Choose File…") {
                                    pickConfigPlist()
                                }
                            }
                        }
                        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
                            handleDrop(providers: providers)
                        }

                        if let ocErr = model.ocError {
                            Text(ocErr).font(.caption).foregroundColor(.red)
                        }
                        if let rep = model.ocReport {
                            HStack {
                                Label(
                                    rep.valid ? "PASS: Config matches \(rep.mode.rawValue) rules" : "INVALID: \(rep.errorCount) error(s), \(rep.warnCount) warning(s)",
                                    systemImage: rep.valid ? "checkmark.seal.fill" : "xmark.octagon.fill"
                                )
                                .foregroundColor(rep.valid ? .green : .red)
                                .bold()
                                Spacer()
                                Text(rep.configPath).font(.caption2).foregroundColor(.secondary)
                            }
                            ForEach(rep.findings.indices, id: \.self) { idx in
                                let f = rep.findings[idx]
                                HStack(alignment: .top, spacing: 6) {
                                    Text("[\(f.severity.rawValue)]")
                                        .font(.system(.caption2, design: .monospaced))
                                        .foregroundColor(f.severity == .pass ? .green : (f.severity == .warn ? .orange : .red))
                                    Text(f.keyPath).font(.system(.caption2, design: .monospaced)).bold()
                                    Text(f.message).font(.caption2).foregroundColor(.secondary)
                                }
                            }
                        }
                    }
                    .padding(6)
                }
            }
            .padding()
        }
    }

    private func chooseAndExportPCIBarCompat() {
        guard let pURL = model.payloadURL else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Choose destination folder for GarnetPCIBarCompat.kext (e.g. Desktop or EFI/OC/Kexts)"
        if panel.runModal() == .OK, let dstDir = panel.url {
            do {
                let dst = try SystemDiskInstaller.exportPCIBarCompat(fromPayload: pURL, toDestination: dstDir)
                exportStatus = "Exported to \(dst.path)"
            } catch {
                exportStatus = "Export failed: \(error)"
            }
        }
    }

    private func pickConfigPlist() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [UTType.propertyList]
        panel.prompt = "Inspect Read-Only"
        if panel.runModal() == .OK, let u = panel.url {
            model.validateOpenCoreConfig(at: u)
        }
    }

    private func handleDrop(providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first(where: { $0.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) }) else {
            return false
        }
        provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
            guard let data = item as? Data,
                  let url = URL(dataRepresentation: data, relativeTo: nil) else { return }
            Task { @MainActor in
                model.validateOpenCoreConfig(at: url)
            }
        }
        return true
    }
}
