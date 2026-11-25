import Foundation
#if canImport(GarnetCore)
import GarnetCore
#endif

@main
struct GarnetCLI {
    static func main() {
        let code = runCLI(Array(CommandLine.arguments.dropFirst()))
        Foundation.exit(code)
    }

    static func runCLI(_ args: [String]) -> Int32 {
        guard let cmd = args.first else {
            printUsage()
            return 2
        }
        if cmd == "-h" || cmd == "--help" || cmd == "help" {
            printUsage()
            return 0
        }

        let rest = Array(args.dropFirst())
        do {
            switch cmd {
            case "preflight":
                return try cmdPreflight(rest)
            case "validate-oc":
                return try cmdValidateOC(rest)
            case "configure-oc":
                return try cmdConfigureOC(rest)
            case "install":
                return try cmdInstall(rest)
            case "uninstall":
                return try cmdUninstall(rest)
            case "status":
                return try cmdStatus(rest)
            case "denylist":
                return try cmdDenylist(rest)
            case "export-pcibarcompat":
                return try cmdExportPCIBarCompat(rest)
            case "export-diag":
                return try cmdExportDiag(rest)
            default:
                fputs("error: unknown subcommand '\(cmd)'\n", stderr)
                printUsage()
                return 2
            }
        } catch {
            fputs("error: \(error)\n", stderr)
            return 1
        }
    }

    // MARK: - Option Parsing Helpers

    private struct ParsedArgs {
        var flags: Set<String> = []
        var options: [String: String] = [:]
        var positional: [String] = []
    }

    private static func parse(_ args: [String], valueFlags: Set<String>, boolFlags: Set<String>) throws -> ParsedArgs {
        var out = ParsedArgs()
        var i = 0
        while i < args.count {
            let a = args[i]
            if valueFlags.contains(a) {
                guard i + 1 < args.count else {
                    throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "Missing value for \(a)"])
                }
                out.options[a] = args[i + 1]
                i += 2
            } else if boolFlags.contains(a) {
                out.flags.insert(a)
                i += 1
            } else if a.hasPrefix("--") {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "Unknown flag \(a)"])
            } else {
                out.positional.append(a)
                i += 1
            }
        }
        return out
    }

    private static func resolvePayloadURL(explicit: String?) throws -> URL {
        if let explicit, !explicit.isEmpty {
            return URL(fileURLWithPath: explicit).standardizedFileURL
        }
        let exeURL = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL.resolvingSymlinksInPath()
        let exeDir = exeURL.deletingLastPathComponent()
        let candidates = [
            exeDir.appendingPathComponent("Payload"),
            exeDir.appendingPathComponent("../Resources/Payload").standardizedFileURL,
            exeDir.appendingPathComponent("payload"),
            exeDir.appendingPathComponent("../Resources/payload").standardizedFileURL
        ]
        for c in candidates {
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: c.appendingPathComponent("MANIFEST.txt").path, isDirectory: &isDir), !isDir.boolValue {
                return c
            }
        }
        if let resURL = Bundle.main.resourceURL?.appendingPathComponent("Payload"),
           FileManager.default.fileExists(atPath: resURL.appendingPathComponent("MANIFEST.txt").path) {
            return resURL
        }
        throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "Cannot locate embedded Payload; pass --payload <dir>"])
    }

    private static func buildEnv(sysRoot: String?, runner: CommandRunning = ProcessCommandRunner()) -> HostEnvironment {
        let rootURL = URL(fileURLWithPath: (sysRoot?.isEmpty == false ? sysRoot! : "/")).standardizedFileURL
        return HostEnvironment(sysRoot: rootURL, runner: runner)
    }

    private static func resolveSigVerify(noCodesignVerify: Bool, env: HostEnvironment) throws -> Bool {
        if noCodesignVerify {
            guard env.sysRoot.path != "/" else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "--no-codesign-verify is forbidden against real / sysroot"])
            }
            return false
        }
        return true
    }

    private static func resolveAllowNonRootSandbox(_ flag: Bool, env: HostEnvironment) throws -> Bool {
        if flag {
            guard env.sysRoot.path != "/" else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "--allow-nonroot-sandbox is forbidden against real / sysroot"])
            }
            return true
        }
        return false
    }

    private static func emitJSON<T: Encodable>(_ value: T) throws {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        let data = try enc.encode(value)
        if let s = String(data: data, encoding: .utf8) {
            print(s)
        }
    }

    // MARK: - Subcommands

    private static func cmdPreflight(_ args: [String]) throws -> Int32 {
        let parsed = try parse(args, valueFlags: ["--payload", "--sysroot"], boolFlags: ["--json", "--no-codesign-verify"])
        let env = buildEnv(sysRoot: parsed.options["--sysroot"])
        let payloadURL = try resolvePayloadURL(explicit: parsed.options["--payload"])
        let verifySig = try resolveSigVerify(noCodesignVerify: parsed.flags.contains("--no-codesign-verify"), env: env)

        let verifiedPayload = try PayloadVerifier.verify(payloadURL: payloadURL, runner: env.runner, verifyCodeSignatures: verifySig)
        let report = PreflightChecker.run(env: env, payloadURL: payloadURL)

        if parsed.flags.contains("--json") {
            struct PayloadAndPreflight: Encodable {
                let payload: VerifiedPayloadInfo
                let preflight: PreflightReport
            }
            try emitJSON(PayloadAndPreflight(payload: verifiedPayload, preflight: report))
        } else {
            print("Payload: Garnet \(verifiedPayload.garnetVersion) (files=\(verifiedPayload.fileCount), chip=\(verifiedPayload.targetSilicon) [\(verifiedPayload.targetPCIIDs)])")
            print("Overall Status: \(report.overallStatus.rawValue) (canInstall=\(report.canInstall))")
            for item in report.checks {
                print("  [\(item.level)] \(item.name): \(item.detail)")
            }
            print("BAR Recommendation: \(report.barRecommendation.rawValue) — \(report.barReason)")
            print("BIOS Reminder: \(report.biosReminder)")
        }

        switch report.overallStatus {
        case .ready: return 0
        case .sipSetupRequired: return 3
        case .blocked: return 4
        }
    }

    private static func cmdValidateOC(_ args: [String]) throws -> Int32 {
        let parsed = try parse(args, valueFlags: ["--config", "--mode"], boolFlags: ["--json"])
        guard let cfgPath = parsed.options["--config"] ?? parsed.positional.first else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "validate-oc requires --config <path>"])
        }
        let modeStr = parsed.options["--mode"] ?? "full"
        guard let mode = OpenCoreMode(rawValue: modeStr) else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "Invalid --mode '\(modeStr)' (expected full or small)"])
        }
        let report = try OpenCoreValidator.validate(configURL: URL(fileURLWithPath: cfgPath), mode: mode)
        if parsed.flags.contains("--json") {
            try emitJSON(report)
        } else {
            print("OpenCore config: \(report.configPath) (mode=\(report.mode.rawValue), valid=\(report.valid), errors=\(report.errorCount), warnings=\(report.warnCount), readOnly=\(report.readOnlyVerified))")
            for f in report.findings {
                print("  [\(f.severity.rawValue)] \(f.keyPath): \(f.message)")
            }
        }
        return report.valid ? 0 : 5
    }

    private static func cmdConfigureOC(_ args: [String]) throws -> Int32 {
        let parsed = try parse(args, valueFlags: ["--config", "--mode", "--payload"], boolFlags: ["--json"])
        guard let targetPath = parsed.options["--config"] ?? parsed.positional.first else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "configure-oc requires --config <EFI-or-config.plist>"])
        }
        let modeStr = parsed.options["--mode"] ?? "full"
        guard let mode = OpenCoreMode(rawValue: modeStr) else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "Invalid --mode '\(modeStr)' (expected full or small)"])
        }
        let payloadURL: URL? = (mode == .full) ? try resolvePayloadURL(explicit: parsed.options["--payload"]) : nil
        let result = try OpenCoreValidator.configure(
            targetURL: URL(fileURLWithPath: targetPath),
            mode: mode,
            payloadURL: payloadURL
        )
        if parsed.flags.contains("--json") {
            try emitJSON(result)
        } else {
            print("Configured OpenCore: \(result.configPath) (mode=\(result.mode.rawValue), valid=\(result.report.valid))")
            print("  Backup: \(result.backupPath)")
            if let kextPath = result.kextInstalledPath {
                print("  Kext: \(kextPath)")
            }
        }
        return result.report.valid ? 0 : 5
    }

    private static func cmdInstall(_ args: [String]) throws -> Int32 {
        let parsed = try parse(
            args,
            valueFlags: ["--payload", "--sysroot"],
            boolFlags: ["--with-vulkan", "--dry-run", "--json", "--no-codesign-verify", "--allow-nonroot-sandbox"]
        )
        let env = buildEnv(sysRoot: parsed.options["--sysroot"])
        let payloadURL = try resolvePayloadURL(explicit: parsed.options["--payload"])
        let verifySig = try resolveSigVerify(noCodesignVerify: parsed.flags.contains("--no-codesign-verify"), env: env)
        let allowSandbox = try resolveAllowNonRootSandbox(parsed.flags.contains("--allow-nonroot-sandbox"), env: env)

        let opts = InstallOptions(
            installVulkanICD: parsed.flags.contains("--with-vulkan"),
            dryRun: parsed.flags.contains("--dry-run"),
            verifyCodeSignatures: verifySig,
            allowNonRootSandbox: allowSandbox
        )
        let res = try SystemDiskInstaller.install(payloadURL: payloadURL, options: opts, env: env)
        if parsed.flags.contains("--json") {
            try emitJSON(res)
        } else {
            print(res.message)
            print("  Layout: \(res.matchedShortId) (\(res.matchedLayoutId))")
            print("  Vulkan ICD: \(res.vulkanICDInstalled ? "enabled" : "disabled (default)")")
            print("  Foreign AuxKC kexts preserved: \(res.preservedForeignAuxKexts.count)")
            for fk in res.preservedForeignAuxKexts {
                print("    - \(fk.bundleID): \(fk.bundlePath)")
            }
            if let bak = res.backupDirectory {
                print("  Backup: \(bak)")
            }
        }
        return 0
    }

    private static func cmdUninstall(_ args: [String]) throws -> Int32 {
        let parsed = try parse(
            args,
            valueFlags: ["--sysroot"],
            boolFlags: ["--remove-denylist", "--dry-run", "--json", "--allow-nonroot-sandbox"]
        )
        let env = buildEnv(sysRoot: parsed.options["--sysroot"])
        let allowSandbox = try resolveAllowNonRootSandbox(parsed.flags.contains("--allow-nonroot-sandbox"), env: env)
        let opts = UninstallOptions(
            removeDenylist: parsed.flags.contains("--remove-denylist"),
            dryRun: parsed.flags.contains("--dry-run"),
            allowNonRootSandbox: allowSandbox
        )
        let res = try SystemDiskInstaller.uninstall(options: opts, env: env)
        if parsed.flags.contains("--json") {
            try emitJSON(res)
        } else {
            print(res.message)
            print("  Denylist preserved: \(res.preservedDenylist)")
            print("  Foreign AuxKC kexts preserved: \(res.preservedForeignAuxKexts.count)")
            print("  Paths removed/planned: \(res.removedPaths.count)")
        }
        return 0
    }

    private static func cmdStatus(_ args: [String]) throws -> Int32 {
        let parsed = try parse(args, valueFlags: ["--sysroot"], boolFlags: ["--json"])
        let env = buildEnv(sysRoot: parsed.options["--sysroot"])
        let rep = DiagnosticsManager.collectStatus(env: env)
        if parsed.flags.contains("--json") {
            try emitJSON(rep)
        } else {
            print("Kexts Loaded: Garnet=\(rep.driverLoaded), GarnetAccel=\(rep.accelLoaded), GarnetPCIBarCompat=\(rep.pciBarCompatLoaded)")
            print("Installed Version: \(rep.installedVersion ?? "none")")
            print("BAR0 Size: \(rep.bar0CurrentMB.map { "\($0) MB" } ?? "unknown") | Display: \(rep.displayPrimaryStatus ?? "unknown")")
            print("GarnetPCIBarCompat: mode=\(rep.iopciCompatMatchMode ?? "none"), IOPCIFamilyUUID=\(rep.iopciCompatUUID ?? "unknown")")
            print("SIP Ready For AuxKC: \(rep.sipReadyForAuxKC) | Vulkan ICD: \(rep.vulkanInstalled ? "installed" : "not installed") | Denylist Entries: \(rep.denylistCount)")
            if let vram = rep.vramStatSummary {
                print("VRAM Summary: \(vram)")
            }
        }
        return 0
    }

    private static func cmdDenylist(_ args: [String]) throws -> Int32 {
        guard let sub = args.first else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "denylist requires subcommand: list | add-app | add-bundle | add-proc | remove-bundle | remove-proc | clear | sync"])
        }
        let parsed = try parse(
            Array(args.dropFirst()),
            valueFlags: ["--sysroot"],
            boolFlags: ["--json", "--no-proc-fallback", "--no-sync"]
        )
        let env = buildEnv(sysRoot: parsed.options["--sysroot"])
        let syncLive = !parsed.flags.contains("--no-sync")

        switch sub {
        case "list":
            let list = try DenylistManager.load(env: env)
            if parsed.flags.contains("--json") {
                try emitJSON(list)
            } else {
                for e in list { print(e.rawLine) }
            }
            return 0
        case "add-app":
            guard let appPath = parsed.positional.first else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "denylist add-app requires <path/to/App.app>"])
            }
            let res = try DenylistManager.addAppBundle(
                at: URL(fileURLWithPath: appPath),
                env: env,
                includeProcFallback: !parsed.flags.contains("--no-proc-fallback"),
                syncLiveKernel: syncLive
            )
            if parsed.flags.contains("--json") {
                struct AddAppOut: Encodable {
                    let metadata: AppBundleMetadata
                    let entries: [DenylistEntry]
                }
                try emitJSON(AddAppOut(metadata: res.metadata, entries: res.entries))
            } else {
                print("Added app \(res.metadata.appPath) (bundle=\(res.metadata.bundleIdentifier ?? "none"), proc=\(res.metadata.executableName ?? "none"))")
            }
            return 0
        case "add-bundle":
            guard let bid = parsed.positional.first else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "denylist add-bundle requires <CFBundleIdentifier>"])
            }
            let entries = try DenylistManager.add(kind: .bundle, value: bid, env: env, syncLiveKernel: syncLive)
            if parsed.flags.contains("--json") { try emitJSON(entries) } else { print("Added bundle:\(bid)") }
            return 0
        case "add-proc":
            guard let proc = parsed.positional.first else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "denylist add-proc requires <process-name>"])
            }
            let entries = try DenylistManager.add(kind: .proc, value: proc, env: env, syncLiveKernel: syncLive)
            if parsed.flags.contains("--json") { try emitJSON(entries) } else { print("Added proc:\(proc)") }
            return 0
        case "remove-bundle":
            guard let bid = parsed.positional.first else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "denylist remove-bundle requires <CFBundleIdentifier>"])
            }
            let entries = try DenylistManager.remove(kind: .bundle, value: bid, env: env, syncLiveKernel: syncLive)
            if parsed.flags.contains("--json") { try emitJSON(entries) } else { print("Removed bundle:\(bid)") }
            return 0
        case "remove-proc":
            guard let proc = parsed.positional.first else {
                throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "denylist remove-proc requires <process-name>"])
            }
            let entries = try DenylistManager.remove(kind: .proc, value: proc, env: env, syncLiveKernel: syncLive)
            if parsed.flags.contains("--json") { try emitJSON(entries) } else { print("Removed proc:\(proc)") }
            return 0
        case "clear":
            try DenylistManager.clear(env: env, syncLiveKernel: syncLive)
            if parsed.flags.contains("--json") { try emitJSON([DenylistEntry]()) } else { print("Cleared denylist") }
            return 0
        case "sync":
            DenylistManager.syncAllToLiveKernel(env: env)
            let entries = try DenylistManager.load(env: env)
            if parsed.flags.contains("--json") { try emitJSON(entries) } else { print("Synced proc entries to live kernel") }
            return 0
        default:
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "Unknown denylist subcommand '\(sub)'"])
        }
    }

    private static func cmdExportPCIBarCompat(_ args: [String]) throws -> Int32 {
        let parsed = try parse(args, valueFlags: ["--payload", "--out"], boolFlags: ["--json"])
        guard let outPath = parsed.options["--out"] ?? parsed.positional.first else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "export-pcibarcompat requires --out <dest-dir>"])
        }
        let payloadURL = try resolvePayloadURL(explicit: parsed.options["--payload"])
        let dst = try SystemDiskInstaller.exportPCIBarCompat(
            fromPayload: payloadURL,
            toDestination: URL(fileURLWithPath: outPath)
        )
        if parsed.flags.contains("--json") {
            try emitJSON(["exportedPath": dst.path])
        } else {
            print("Exported GarnetPCIBarCompat.kext to: \(dst.path)")
            print("Place into OpenCore EFI/OC/Kexts/ manually; do not copy to /Library/Extensions/.")
        }
        return 0
    }

    private static func cmdExportDiag(_ args: [String]) throws -> Int32 {
        let parsed = try parse(args, valueFlags: ["--out", "--payload", "--sysroot"], boolFlags: ["--json"])
        guard let outPath = parsed.options["--out"] ?? parsed.positional.first else {
            throw NSError(domain: "garnetctl", code: 2, userInfo: [NSLocalizedDescriptionKey: "export-diag requires --out <archive.tar.gz>"])
        }
        let env = buildEnv(sysRoot: parsed.options["--sysroot"])
        let payloadURL = parsed.options["--payload"].map { URL(fileURLWithPath: $0) }
        let archiveURL = try DiagnosticsManager.exportSanitizedDiagnostics(
            toArchive: URL(fileURLWithPath: outPath),
            env: env,
            payloadURL: payloadURL
        )
        if parsed.flags.contains("--json") {
            try emitJSON(["archivePath": archiveURL.path])
        } else {
            print("Exported sanitized diagnostics bundle to: \(archiveURL.path)")
        }
        return 0
    }

    private static func printUsage() {
        print("""
        Usage: garnetctl <command> [options]
          preflight [--payload <dir>] [--sysroot <dir>] [--json]
          validate-oc --config <config.plist> [--mode full|small] [--json]
          configure-oc --config <EFI-or-config.plist> [--mode full|small] [--payload <dir>] [--json]
          install [--payload <dir>] [--with-vulkan] [--dry-run] [--json]
          uninstall [--remove-denylist] [--dry-run] [--json]
          status [--json]
          denylist <list|add-app|add-bundle|add-proc|remove-bundle|remove-proc|clear|sync> [...]
          export-pcibarcompat [--payload <dir>] --out <dest-dir> [--json]
          export-diag --out <archive.tar.gz> [--payload <dir>] [--json]
        """)
    }
}
