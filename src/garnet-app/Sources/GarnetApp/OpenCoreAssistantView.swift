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
                Text("Garnet never mounts your EFI System Partition automatically. Choose your mounted EFI folder or config.plist to back up, update, and validate it.")
                    .font(.caption)
                    .foregroundColor(.secondary)

                GroupBox("1. Automatic OpenCore Configuration") {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Picker("Target Mode", selection: $model.selectedOCMode) {
                                Text("16 GiB Full-BAR (Default)").tag(OpenCoreMode.mode16g)
                                Text("256 MiB Small-BAR Fallback").tag(OpenCoreMode.modeSmall)
                            }
                            .pickerStyle(.segmented)
                            .frame(maxWidth: 420)

                            Spacer()

                            Button {
                                pickAndConfigureOpenCore()
                            } label: {
                                Label("Auto-Configure OpenCore…", systemImage: "wand.and.stars")
                            }
                            .disabled(model.selectedOCMode == .mode16g && model.payloadURL == nil)
                        }

                        Text("Creates a timestamped config.plist backup, copies GarnetPCIBarCompat.kext to OC/Kexts (16G mode), updates required OpenCore keys, and validates the output.")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        if let res = model.ocConfigureResult {
                            VStack(alignment: .leading, spacing: 4) {
                                Text("Updated: \(res.configPath)")
                                    .font(.caption).foregroundColor(.green)
                                Text("Backup: \(res.backupPath)")
                                    .font(.caption2).foregroundColor(.secondary)
                                if let kextPath = res.kextInstalledPath {
                                    Text("Installed kext: \(kextPath)")
                                        .font(.caption2).foregroundColor(.secondary)
                                }
                            }
                        }

                        if let ocErr = model.ocError {
                            Text(ocErr).font(.caption).foregroundColor(.red)
                        }
                        if let rep = model.ocReport {
                            validationReportView(rep)
                        }
                    }
                    .padding(6)
                }

                GroupBox("2. Read-Only config.plist Inspector (Drag & Drop or Choose File)") {
                    VStack(alignment: .leading, spacing: 10) {
                        ZStack {
                            RoundedRectangle(cornerRadius: 8)
                                .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6]))
                                .foregroundColor(dropTargeted ? .accentColor : .secondary.opacity(0.5))
                                .frame(height: 68)
                            HStack(spacing: 12) {
                                Image(systemName: "doc.badge.gearshape")
                                    .font(.title2)
                                Text("Drop config.plist here for a read-only check")
                                    .font(.caption)
                                Button("Inspect Read-Only…") {
                                    pickConfigPlist()
                                }
                            }
                        }
                        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
                            handleDrop(providers: providers)
                        }
                    }
                    .padding(6)
                }

                GroupBox("3. Manual Export & Reference Snippets") {
                    VStack(alignment: .leading, spacing: 8) {
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
                        .frame(height: 140)
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
            }
            .padding()
        }
    }

    @ViewBuilder
    private func validationReportView(_ rep: OpenCoreReport) -> some View {
        VStack(alignment: .leading, spacing: 6) {
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

    private func pickAndConfigureOpenCore() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowedContentTypes = [UTType.propertyList, UTType.folder]
        panel.prompt = "Configure"
        panel.message = "Select your mounted EFI folder, OC folder, or config.plist"
        if panel.runModal() == .OK, let u = panel.url {
            model.configureOpenCore(at: u)
        }
    }

    private func chooseAndExportPCIBarCompat() {
        guard let pURL = model.payloadURL else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Choose destination folder for GarnetPCIBarCompat.kext (e.g. EFI/OC/Kexts)"
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
