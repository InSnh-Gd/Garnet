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

    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 4) {
                Text("OpenCore Configuration Assistant")
                    .font(.title2).bold()
                Text("Mount your EFI partition first. Garnet only reads or updates a config.plist you choose.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
            Divider()

            Form {
                Section("OpenCore Configuration") {
                    HStack {
                        Picker("Target Mode", selection: $model.selectedOCMode) {
                            Text(OpenCoreMode.full.displayName).tag(OpenCoreMode.full)
                            Text(OpenCoreMode.modeSmall.displayName).tag(OpenCoreMode.modeSmall)
                        }
                        .pickerStyle(.segmented)
                        .frame(maxWidth: 420)

                        Spacer()

                        Button {
                            pickAndConfigureOpenCore()
                        } label: {
                            Label("Configure OpenCore…", systemImage: "slider.horizontal.3")
                        }
                        .disabled(model.selectedOCMode == .full && model.payloadURL == nil)
                    }

                    Text("Backs up config.plist, copies GarnetPCIBarCompat.kext (Full-BAR mode), writes required keys, and validates.")
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

                Section("Read-Only config.plist Inspector") {
                    FileDropRow(
                        systemImage: "doc.badge.gearshape",
                        message: "Drop config.plist here for a read-only check",
                        buttonTitle: "Inspect Read-Only…",
                        onPick: pickConfigPlist,
                        onDropURL: { model.validateOpenCoreConfig(at: $0) }
                    )
                }

                Section("Manual Export & Reference") {
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
                            copiedBanner = "Copied \(model.selectedOCMode.displayName) snippet"
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
            }
            .formStyle(.grouped)
            .onChange(of: model.selectedOCMode) {
                copiedBanner = nil
            }
        }
    }

    @ViewBuilder
    private func validationReportView(_ rep: OpenCoreReport) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label(
                    rep.valid ? "PASS: Config matches \(rep.mode.displayName) rules" : "INVALID: \(rep.errorCount) error(s), \(rep.warnCount) warning(s)",
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
}
