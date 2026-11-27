import SwiftUI
import AppKit
import Foundation
#if canImport(GarnetCore)
import GarnetCore
#endif

@main
@MainActor
final class GarnetAppDelegate: NSObject, NSApplicationDelegate, NSToolbarDelegate {
    private var window: NSWindow?
    private var splitViewController: NSSplitViewController?
    private let model = GarnetAppModel()

    static func main() {
        let app = NSApplication.shared
        let delegate = GarnetAppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.run()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMainMenu()
        showMainWindow()
        NSApp.activate(ignoringOtherApps: true)
        model.refreshAll()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    private func showMainWindow() {
        let sidebarVC = NSHostingController(rootView: SidebarView(model: model))
        sidebarVC.sizingOptions = []
        let sidebarItem = NSSplitViewItem(sidebarWithViewController: sidebarVC)
        sidebarItem.minimumThickness = 190
        sidebarItem.maximumThickness = 260
        sidebarItem.preferredThicknessFraction = 0.22
        sidebarItem.canCollapse = true
        sidebarItem.collapseBehavior = .preferResizingSiblingsWithFixedSplitView
        sidebarItem.allowsFullHeightLayout = true
        sidebarItem.titlebarSeparatorStyle = .none

        let detailVC = NSHostingController(rootView: DetailView(model: model))
        detailVC.sizingOptions = []
        let detailItem = NSSplitViewItem(viewController: detailVC)
        detailItem.minimumThickness = 520
        detailItem.titlebarSeparatorStyle = .automatic

        let svc = NSSplitViewController()
        svc.addSplitViewItem(sidebarItem)
        svc.addSplitViewItem(detailItem)
        self.splitViewController = svc

        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 960, height: 680),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        win.title = "Garnet"
        win.minSize = NSSize(width: 760, height: 580)
        win.toolbarStyle = .unified
        win.contentViewController = svc

        let toolbar = NSToolbar(identifier: "GarnetMainToolbar")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        toolbar.allowsUserCustomization = false
        win.toolbar = toolbar

        win.center()
        win.makeKeyAndOrderFront(nil)
        self.window = win
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator]
    }

    private func buildMainMenu() {
        let mainMenu = NSMenu()

        let appMenuItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About Garnet", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide Garnet", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let hideOthers = appMenu.addItem(withTitle: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h")
        hideOthers.keyEquivalentModifierMask = [.command, .option]
        appMenu.addItem(withTitle: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit Garnet", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appMenuItem.submenu = appMenu
        mainMenu.addItem(appMenuItem)

        let editMenuItem = NSMenuItem()
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editMenuItem.submenu = editMenu
        mainMenu.addItem(editMenuItem)

        let viewMenuItem = NSMenuItem()
        let viewMenu = NSMenu(title: "View")
        let toggleItem = viewMenu.addItem(withTitle: "Toggle Sidebar", action: #selector(NSSplitViewController.toggleSidebar(_:)), keyEquivalent: "s")
        toggleItem.keyEquivalentModifierMask = [.command, .control]
        viewMenuItem.submenu = viewMenu
        mainMenu.addItem(viewMenuItem)

        let windowMenuItem = NSMenuItem()
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        windowMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        windowMenuItem.submenu = windowMenu
        mainMenu.addItem(windowMenuItem)

        NSApp.mainMenu = mainMenu
        NSApp.windowsMenu = windowMenu
    }
}

struct SidebarView: View {
    @ObservedObject var model: GarnetAppModel

    var body: some View {
        List(GarnetSection.allCases, selection: $model.selectedSection) { sec in
            Label(sec.title, systemImage: sec.systemImage)
                .tag(sec)
        }
        .listStyle(.sidebar)
    }
}

struct DetailView: View {
    @ObservedObject var model: GarnetAppModel

    var body: some View {
        Group {
            switch model.selectedSection ?? .install {
            case .install:
                InstallView(model: model)
            case .opencore:
                OpenCoreAssistantView(model: model)
            case .status:
                StatusTroubleshootView(model: model)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
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
    @Published var isExportingDiag: Bool = false
    @Published var diagExportMessage: String = ""
    @Published var diagExportFailed: Bool = false
    @Published var lastExportedDiagURL: URL?

    @Published var selectedOCMode: OpenCoreMode = .full
    @Published var selectedAMFIMode: OpenCoreAMFIMode = .legacy
    @Published var ocReport: OpenCoreReport?
    @Published var ocConfigureResult: OpenCoreConfigureResult?
    @Published var ocRestoreResult: OpenCoreRestoreResult?
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
                    pErr = "Embedded driver payload not found in application bundle."
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
                self.selectedOCMode = (pf.barRecommendation == .fullBar) ? .full : .modeSmall
            }
            if let v = snap.vInfo {
                self.selectedAMFIMode = v.trustAnchorEnabled ? .anchor : .legacy
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
        ocRestoreResult = nil
        do {
            ocReport = try OpenCoreValidator.validate(configURL: url, mode: selectedOCMode, amfiMode: selectedAMFIMode)
        } catch {
            ocReport = nil
            ocError = "Failed to inspect plist read-only: \(error)"
        }
    }

    func configureOpenCore(at url: URL) {
        ocError = nil
        ocConfigureResult = nil
        ocRestoreResult = nil
        do {
            let result = try OpenCoreValidator.configure(
                targetURL: url,
                mode: selectedOCMode,
                amfiMode: selectedAMFIMode,
                payloadURL: payloadURL
            )
            ocConfigureResult = result
            ocReport = result.report
        } catch {
            ocError = "Failed to configure OpenCore: \(error)"
        }
    }

    func restoreOpenCore(at url: URL, stripOnly: Bool = false) {
        ocError = nil
        ocConfigureResult = nil
        ocRestoreResult = nil
        ocReport = nil
        do {
            ocRestoreResult = try OpenCoreValidator.restore(targetURL: url, backupURL: nil, stripOnly: stripOnly)
        } catch {
            ocError = "Failed to restore OpenCore: \(error)"
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

    func exportDiagnosticsOneClick() {
        exportDiagnostics(to: DiagnosticsManager.defaultDiagnosticsArchiveURL(), revealInFinder: true)
    }

    func exportDiagnostics(to outURL: URL, revealInFinder: Bool = true) {
        guard !isExportingDiag else { return }
        isExportingDiag = true
        diagExportFailed = false
        diagExportMessage = "Collecting diagnostics..."
        let currentEnv = env
        let pURL = payloadURL
        let logSnapshot = actionLog

        Task { [weak self] in
            let outcome: Result<URL, Error> = await Task.detached {
                do {
                    let saved = try DiagnosticsManager.exportSanitizedDiagnostics(
                        toArchive: outURL,
                        env: currentEnv,
                        payloadURL: pURL,
                        appActionLog: logSnapshot
                    )
                    return .success(saved)
                } catch {
                    return .failure(error)
                }
            }.value

            guard let self else { return }
            self.isExportingDiag = false
            switch outcome {
            case .success(let saved):
                self.lastExportedDiagURL = saved
                self.diagExportFailed = false
                self.diagExportMessage = "Saved to \(saved.path)"
                self.appendLog("Exported diagnostics bundle to \(saved.path)")
                if revealInFinder {
                    NSWorkspace.shared.activateFileViewerSelecting([saved])
                }
            case .failure(let error):
                self.diagExportFailed = true
                self.diagExportMessage = "Export failed: \(error)"
                self.appendLog("Diagnostics export failed: \(error)")
            }
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
