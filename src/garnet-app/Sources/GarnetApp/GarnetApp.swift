import SwiftUI
import AppKit
import Foundation
#if canImport(GarnetCore)
import GarnetCore
#endif

@main
struct GarnetInstallerApp: App {
    @StateObject private var model = GarnetAppModel()

    var body: some Scene {
        WindowGroup("Garnet RDNA 4 Driver Manager") {
            MainContentView(model: model)
                .frame(minWidth: 880, minHeight: 620)
        }
        .windowResizability(.contentSize)
    }
}

struct MainContentView: View {
    @ObservedObject var model: GarnetAppModel

    var body: some View {
        NavigationSplitView {
            List(GarnetSection.allCases, selection: $model.selectedSection) { sec in
                Label(sec.title, systemImage: sec.systemImage)
                    .tag(sec)
            }
            .navigationTitle("Garnet")
            .frame(minWidth: 210)
        } detail: {
            switch model.selectedSection ?? .install {
            case .install:
                InstallView(model: model)
            case .opencore:
                OpenCoreAssistantView(model: model)
            case .status:
                StatusTroubleshootView(model: model)
            }
        }
        .onAppear {
            model.refreshAll()
        }
    }
}

enum GarnetSection: String, CaseIterable, Identifiable {
    case install
    case opencore
    case status

    var id: String { rawValue }

    var title: String {
        switch self {
        case .install: return "Install / Uninstall"
        case .opencore: return "OpenCore Assistant"
        case .status: return "Status & Denylist"
        }
    }

    var systemImage: String {
        switch self {
        case .install: return "arrow.down.circle"
        case .opencore: return "checklist"
        case .status: return "stethoscope"
        }
    }
}

@MainActor
final class GarnetAppModel: ObservableObject {
    @Published var selectedSection: GarnetSection? = .install
    @Published var payloadURL: URL?
    @Published var verifiedPayload: VerifiedPayloadInfo?
    @Published var payloadError: String?
    @Published var preflight: PreflightReport?
    @Published var runtimeStatus: RuntimeStatusReport?
    @Published var denylistEntries: [DenylistEntry] = []

    @Published var installVulkanICD: Bool = false
    @Published var removeDenylistOnUninstall: Bool = false
    @Published var isBusy: Bool = false
    @Published var actionLog: String = ""

    @Published var selectedOCMode: OpenCoreMode = .mode16g
    @Published var ocReport: OpenCoreReport?
    @Published var ocConfigureResult: OpenCoreConfigureResult?
    @Published var ocError: String?

    private let env = HostEnvironment()

    init() {
        self.payloadURL = Self.locateEmbeddedPayload()
    }

    static func locateEmbeddedPayload() -> URL? {
        let fm = FileManager.default
        var candidates: [URL] = []
        if let res = Bundle.main.resourceURL {
            candidates.append(res.appendingPathComponent("Payload"))
        }
        let exeDir = Bundle.main.bundleURL
        candidates.append(exeDir.appendingPathComponent("Contents/Resources/Payload"))
        if let dir = Bundle.main.bundleURL.deletingLastPathComponent() as URL? {
            candidates.append(dir.appendingPathComponent("Payload"))
            candidates.append(dir.appendingPathComponent("payload"))
        }
        for c in candidates {
            if fm.fileExists(atPath: c.appendingPathComponent("MANIFEST.txt").path) {
                return c
            }
        }
        return nil
    }

    static func locateBundledCLI() -> URL? {
        let fm = FileManager.default
        if let res = Bundle.main.resourceURL {
            let cli = res.appendingPathComponent("garnetctl")
            if fm.isExecutableFile(atPath: cli.path) {
                return cli
            }
        }
        let sibling = Bundle.main.bundleURL.deletingLastPathComponent().appendingPathComponent("garnetctl")
        if fm.isExecutableFile(atPath: sibling.path) {
            return sibling
        }
        return nil
    }

    func refreshAll() {
        let currentEnv = env
        let pURL = payloadURL
        Task { [weak self] in
            let snap: (vInfo: VerifiedPayloadInfo?, pErr: String?, pf: PreflightReport?, st: RuntimeStatusReport, dl: [DenylistEntry]) = await Task.detached {
                var vInfo: VerifiedPayloadInfo?
                var pErr: String?
                var pf: PreflightReport?
                if let u = pURL {
                    do {
                        vInfo = try PayloadVerifier.verify(payloadURL: u, runner: currentEnv.runner, verifyCodeSignatures: true)
                        pf = PreflightChecker.run(env: currentEnv, payloadURL: u)
                    } catch {
                        pErr = "\(error)"
                    }
                } else {
                    pErr = "Embedded Release 1 payload not found in application bundle."
                }
                let st = DiagnosticsManager.collectStatus(env: currentEnv)
                let dl = (try? DenylistManager.load(env: currentEnv)) ?? []
                return (vInfo, pErr, pf, st, dl)
            }.value

            guard let self else { return }
            self.verifiedPayload = snap.vInfo
            self.payloadError = snap.pErr
            self.preflight = snap.pf
            self.runtimeStatus = snap.st
            self.denylistEntries = snap.dl
            if let pf = snap.pf {
                self.selectedOCMode = (pf.barRecommendation == .full16G) ? .mode16g : .modeSmall
            }
        }
    }

    func runInstall(dryRun: Bool) {
        guard let pURL = payloadURL else {
            appendLog("ERROR: Embedded payload missing.")
            return
        }
        var args = ["install", "--payload", pURL.path]
        if installVulkanICD { args.append("--with-vulkan") }
        if dryRun { args.append("--dry-run") }
        runCLI(args: args, requiresAdmin: !dryRun, title: dryRun ? "Install Dry-Run" : "Install / Upgrade Driver")
    }

    func runUninstall(dryRun: Bool) {
        var args = ["uninstall"]
        if removeDenylistOnUninstall { args.append("--remove-denylist") }
        if dryRun { args.append("--dry-run") }
        runCLI(args: args, requiresAdmin: !dryRun, title: dryRun ? "Uninstall Dry-Run" : "Uninstall Driver")
    }

    func addDenylistApp(at appURL: URL) {
        runCLI(args: ["denylist", "add-app", appURL.path], requiresAdmin: true, title: "Denylist Add App")
    }

    func addDenylistProc(_ name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return }
        runCLI(args: ["denylist", "add-proc", clean], requiresAdmin: true, title: "Denylist Add Process")
    }

    func removeDenylistEntry(_ entry: DenylistEntry) {
        let cmd = (entry.kind == .bundle) ? "remove-bundle" : "remove-proc"
        runCLI(args: ["denylist", cmd, entry.value], requiresAdmin: true, title: "Denylist Remove")
    }

    func clearDenylist() {
        runCLI(args: ["denylist", "clear"], requiresAdmin: true, title: "Clear Denylist")
    }

    func validateOpenCoreConfig(at url: URL) {
        ocError = nil
        ocConfigureResult = nil
        do {
            ocReport = try OpenCoreValidator.validate(configURL: url, mode: selectedOCMode)
        } catch {
            ocReport = nil
            ocError = "Failed to inspect plist read-only: \(error)"
        }
    }

    func configureOpenCore(at url: URL) {
        ocError = nil
        ocConfigureResult = nil
        do {
            let result = try OpenCoreValidator.configure(targetURL: url, mode: selectedOCMode, payloadURL: payloadURL)
            ocConfigureResult = result
            ocReport = result.report
        } catch {
            ocError = "Failed to configure OpenCore: \(error)"
        }
    }

    func appendLog(_ text: String) {
        let ts = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        let prefix = "[\(ts)] "
        if actionLog.isEmpty {
            actionLog = prefix + text
        } else {
            actionLog += "\n" + prefix + text
        }
    }

    private func runCLI(args: [String], requiresAdmin: Bool, title: String) {
        guard let cliURL = Self.locateBundledCLI() else {
            appendLog("ERROR [\(title)]: Bundled garnetctl executable not found.")
            return
        }
        isBusy = true
        appendLog("Starting \(title): garnetctl \(args.joined(separator: " "))")

        Task { [weak self] in
            let res: CommandResult = await Task.detached {
                let runner = ProcessCommandRunner()
                if requiresAdmin {
                    let cmdLine = ([cliURL.path] + args).map(Self.shellQuote).joined(separator: " ")
                    let appleScript = "do shell script \(Self.appleScriptQuote(cmdLine)) with administrator privileges"
                    return runner.run("/usr/bin/osascript", ["-e", appleScript])
                } else {
                    return runner.run(cliURL.path, args)
                }
            }.value

            guard let self else { return }
            self.isBusy = false
            if !res.stdout.isEmpty {
                self.appendLog(res.stdout.trimmingCharacters(in: .newlines))
            }
            if !res.stderr.isEmpty {
                self.appendLog("stderr: \(res.stderr.trimmingCharacters(in: .newlines))")
            }
            self.appendLog("\(title) exited with code \(res.exitCode).")
            self.refreshAll()
        }
    }

    nonisolated static func shellQuote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    nonisolated static func appleScriptQuote(_ s: String) -> String {
        let escaped = s
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }
}
