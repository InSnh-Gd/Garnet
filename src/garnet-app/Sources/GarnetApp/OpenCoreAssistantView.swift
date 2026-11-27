import SwiftUI
import AppKit
import UniformTypeIdentifiers
#if canImport(GarnetCore)
import GarnetCore
#endif

struct OpenCoreAssistantView: View {
    @ObservedObject var model: GarnetAppModel
    @State private var exportStatus: String = ""
    @State private var exportFailed: Bool = false
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
                        Picker("BAR Mode", selection: $model.selectedOCMode) {
                            Text(OpenCoreMode.full.displayName).tag(OpenCoreMode.full)
                            Text(OpenCoreMode.modeSmall.displayName).tag(OpenCoreMode.modeSmall)
                        }
                        .pickerStyle(.segmented)
                        .frame(maxWidth: 360)

                        Spacer()

                        Button {
                            pickAndConfigureOpenCore()
                        } label: {
                            Label("Configure OpenCore…", systemImage: "slider.horizontal.3")
                        }
                        .disabled(model.selectedOCMode == .full && model.payloadURL == nil)

                        Button(role: .destructive) {
                            pickAndRestoreOpenCore()
                        } label: {
                            Label("Restore OpenCore…", systemImage: "arrow.uturn.backward.circle")
                        }
                    }

                    Picker("AMFI Mode", selection: $model.selectedAMFIMode) {
                        Text(OpenCoreAMFIMode.anchor.displayName).tag(OpenCoreAMFIMode.anchor)
                        Text(OpenCoreAMFIMode.legacy.displayName).tag(OpenCoreAMFIMode.legacy)
                    }
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 420)

                    Text("Configure saves a one-time baseline backup (config.plist.garnet-pre-garnet), installs GarnetPCIBarCompat.kext (Full-BAR), GarnetRecover.efi (Misc -> Tools), and GarnetAnchorInjector.kext (AMFIAnchor mode, requires user Lilu.kext in EFI/OC/Kexts/). Restore reverts to the baseline or strips Garnet entries.")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    if let res = model.ocConfigureResult {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Updated: \(res.configPath) [\(res.mode.displayName), \(res.amfiMode.displayName)]")
                                .font(.caption).foregroundColor(.green)
                            Text("Backup: \(res.backupPath)")
                                .font(.caption2).foregroundColor(.secondary)
                            if let base = res.baselineBackupPath {
                                Text("Baseline backup: \(base)")
                                    .font(.caption2).foregroundColor(.secondary)
                            }
                            if let kextPath = res.kextInstalledPath {
                                Text("Installed kext: \(kextPath)")
                                    .font(.caption2).foregroundColor(.secondary)
                            }
                            if let recPath = res.recoverToolInstalledPath {
                                Text("Installed recovery tool: \(recPath)")
                                    .font(.caption2).foregroundColor(.secondary)
                            }
                            if let injPath = res.anchorInjectorInstalledPath {
                                Text("Installed trust anchor: \(injPath)")
                                    .font(.caption2).foregroundColor(.secondary)
                            }
                        }
                    }

                    if let rest = model.ocRestoreResult {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Restored: \(rest.configPath)")
                                .font(.caption).foregroundColor(.green)
                            if let src = rest.restoredFromBackupPath {
                                Text("Restored from baseline: \(src)")
                                    .font(.caption2).foregroundColor(.secondary)
                            } else {
                                Text("Stripped Garnet entries in-place (no pre-Garnet baseline found)")
                                    .font(.caption2).foregroundColor(.secondary)
                            }
                            Text("Pre-restore backup: \(rest.preRestoreBackupPath)")
                                .font(.caption2).foregroundColor(.secondary)
                            if !rest.removedArtifacts.isEmpty {
                                Text("Removed Garnet EFI files: \(rest.removedArtifacts.count)")
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
                                .foregroundColor(exportFailed ? .red : .green)
                        }

                        Spacer()

                        Button("Copy Snippet") {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(
                                OpenCoreValidator.sampleConfigSnippet(mode: model.selectedOCMode, amfiMode: model.selectedAMFIMode),
                                forType: .string
                            )
                            copiedBanner = "Copied \(model.selectedOCMode.displayName) snippet"
                        }
                        if let copiedBanner {
                            Text(copiedBanner).font(.caption).foregroundColor(.green)
                        }
                    }

                    ScrollView {
                        Text(OpenCoreValidator.sampleConfigSnippet(mode: model.selectedOCMode, amfiMode: model.selectedAMFIMode))
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

    private func pickAndRestoreOpenCore() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowedContentTypes = [UTType.propertyList, UTType.folder]
        panel.prompt = "Restore"
        panel.message = "Select your mounted EFI folder, OC folder, or config.plist to restore from baseline backup or strip Garnet entries"
        if panel.runModal() == .OK, let u = panel.url {
            model.restoreOpenCore(at: u)
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
                exportFailed = false
                exportStatus = "Exported to \(dst.path)"
            } catch {
                exportFailed = true
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
