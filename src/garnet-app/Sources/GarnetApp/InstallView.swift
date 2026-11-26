import SwiftUI
#if canImport(GarnetCore)
import GarnetCore
#endif

struct InstallView: View {
    @ObservedObject var model: GarnetAppModel
    @State private var showUninstallConfirm = false

    var body: some View {
        VStack(spacing: 0) {
            headerBanner
                .padding(.horizontal, 20)
                .padding(.vertical, 14)
            Divider()

            Form {
                Section("Preflight") {
                    if let err = model.payloadError {
                        Label(err, systemImage: "xmark.octagon.fill")
                            .foregroundColor(.red)
                    }
                    if let p = model.verifiedPayload {
                        Text("Payload: Garnet \(p.garnetVersion) | GarnetAccel \(p.accelVersion) | GarnetPCIBarCompat \(p.pcibarCompatVersion) | Target: Navi 44 (\(p.targetPCIIDs))")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    if let pf = model.preflight {
                        ForEach(pf.checks, id: \.name) { item in
                            HStack(alignment: .top, spacing: 8) {
                                Image(systemName: iconName(level: item.level))
                                    .foregroundColor(color(level: item.level))
                                    .frame(width: 18)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(item.name).fontWeight(.medium)
                                    Text(item.detail).font(.caption).foregroundColor(.secondary)
                                }
                                Spacer()
                                Text(item.level)
                                    .font(.caption2)
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(color(level: item.level).opacity(0.15))
                                    .cornerRadius(4)
                            }
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Label(
                                pf.barRecommendation == .fullBar
                                    ? "Recommended OpenCore Mode: \(OpenCoreMode.full.displayName)"
                                    : "Recommended OpenCore Mode: \(OpenCoreMode.modeSmall.displayName)",
                                systemImage: pf.barRecommendation == .fullBar ? "checkmark.shield" : "exclamationmark.triangle"
                            )
                            .fontWeight(.semibold)
                            Text(pf.barReason).font(.caption).foregroundColor(.secondary)
                            Text(pf.biosReminder).font(.caption).foregroundColor(.orange)
                        }

                        if pf.overallStatus == .sipSetupRequired {
                            sipSetupGuide
                        }
                    } else {
                        Text("Running preflight checks...")
                            .foregroundColor(.secondary)
                    }

                    HStack {
                        Button("Re-run Preflight") { model.refreshAll() }
                            .disabled(model.isBusy)
                        Button {
                            model.exportDiagnosticsOneClick()
                        } label: {
                            Label("Export Diagnostics", systemImage: "square.and.arrow.up")
                        }
                        .disabled(model.isExportingDiag)
                        if model.isExportingDiag {
                            ProgressView()
                                .controlSize(.small)
                        }
                        Spacer()
                    }
                    if !model.diagExportMessage.isEmpty {
                        Text(model.diagExportMessage)
                            .font(.caption)
                            .foregroundColor(model.diagExportFailed ? .red : .green)
                            .textSelection(.enabled)
                    }
                }

                Section("Install & Upgrade") {
                    Text("Installs Garnet kexts into /Library/Extensions (rebuilding AuxKC), GarnetMetal.bundle, and translation services. Preserves third-party kexts and migrates v0.1.0 app-denylist.txt into \(GarnetPaths.nativeSupportDir).")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    Toggle("Also install optional Vulkan ICD (\(GarnetPaths.vulkanICDPath) & radv-app)", isOn: $model.installVulkanICD)
                    Text("Default OFF: Metal and OpenGL apps use GarnetMetal directly without exposing an unrequested Vulkan ICD.")
                        .font(.caption2)
                        .foregroundColor(.secondary)

                    HStack(spacing: 12) {
                        Button {
                            model.runInstall(dryRun: true)
                        } label: {
                            Label("Dry-Run Plan", systemImage: "doc.text.magnifyingglass")
                        }
                        .disabled(model.isBusy || model.preflight?.canInstall != true)

                        Button {
                            model.runInstall(dryRun: false)
                        } label: {
                            Label("Install / Upgrade Driver…", systemImage: "square.and.arrow.down")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.isBusy || model.preflight?.canInstall != true)

                        Spacer()
                    }
                }

                Section("Uninstall") {
                    Toggle("Also remove saved app-denylist.txt", isOn: $model.removeDenylistOnUninstall)
                    Text("Removes Garnet components and legacy v0.1.0 runtime files from \(GarnetPaths.nativeSupportDir) and system directories, then rebuilds AuxKC. Reboot and disable Garnet entries in OpenCore if reverting.")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    HStack(spacing: 12) {
                        Button("Uninstall Dry-Run") {
                            model.runUninstall(dryRun: true)
                        }
                        .disabled(model.isBusy)

                        Button(role: .destructive) {
                            showUninstallConfirm = true
                        } label: {
                            Label("Uninstall Garnet…", systemImage: "trash")
                        }
                        .disabled(model.isBusy)

                        Spacer()
                    }
                }

                Section("Execution Log") {
                    ScrollView {
                        Text(model.actionLog.isEmpty ? "No actions executed yet." : model.actionLog)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(height: 140)
                }
            }
            .formStyle(.grouped)
            .confirmationDialog(
                "Remove Garnet from macOS system disk and rebuild AuxKC?",
                isPresented: $showUninstallConfirm
            ) {
                Button("Uninstall Garnet", role: .destructive) {
                    model.runUninstall(dryRun: false)
                }
                Button("Cancel", role: .cancel) {}
            }
        }
    }

    private var headerBanner: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Garnet RX 9060 XT (Navi 44) Installer")
                    .font(.title2).bold()
                Text("Bare-metal x86_64 macOS 26.x — System disk only (ESP untouched)")
                    .font(.subheadline).foregroundColor(.secondary)
            }
            Spacer()
            if let pf = model.preflight {
                statusBadge(pf.overallStatus)
            }
        }
    }

    private var sipSetupGuide: some View {
        VStack(alignment: .leading, spacing: 4) {
            Label("SIP / SecureBoot Setup Required Before AuxKC Install", systemImage: "lock.shield")
                .fontWeight(.semibold)
                .foregroundColor(.orange)
            Text("In OpenCore config.plist set Misc -> Security -> SecureBootModel = Disabled, NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82 -> csr-active-config = <03080000> (0x0803), add csr-active-config to NVRAM -> Delete, then reboot and re-open Garnet.app.")
                .font(.caption)
        }
        .padding(8)
        .background(Color.orange.opacity(0.1))
        .cornerRadius(6)
    }

    private func statusBadge(_ s: PreflightStatus) -> some View {
        let (label, col): (String, Color) = {
            switch s {
            case .ready: return ("READY", .green)
            case .sipSetupRequired: return ("SIP SETUP NEEDED", .orange)
            case .blocked: return ("BLOCKED", .red)
            }
        }()
        return Text(label)
            .font(.caption).bold()
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(col.opacity(0.18))
            .foregroundColor(col)
            .cornerRadius(6)
    }

    private func iconName(level: String) -> String {
        switch level {
        case "PASS": return "checkmark.circle.fill"
        case "WARN": return "exclamationmark.triangle.fill"
        default: return "xmark.octagon.fill"
        }
    }

    private func color(level: String) -> Color {
        switch level {
        case "PASS": return .green
        case "WARN": return .orange
        default: return .red
        }
    }
}
