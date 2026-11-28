import Foundation

public enum SmuTelemetryState: String, Codable, Equatable, Sendable {
    case verified
    case unverified
    case unavailable
}

public struct SmuMetricsParseResult: Equatable, Sendable {
    public let driverIfVersion: String?
    public let layoutVerified: Bool?
    public let valuesPlausible: Bool?
}

public struct RuntimeStatusReport: Codable, Equatable, Sendable {
    public let driverLoaded: Bool
    public let accelLoaded: Bool
    public let pciBarCompatLoaded: Bool
    public let bar0CurrentMB: Int?
    public let displayPrimaryStatus: String?
    public let installedManifestPath: String?
    public let installedVersion: String?
    public let vulkanInstalled: Bool
    public let sipReadyForAuxKC: Bool
    public let bootArgs: String?
    public let iopciCompatMatchMode: String?
    public let iopciCompatUUID: String?
    public let vramStatSummary: String?
    public let denylistCount: Int
    public let smuDriverIfVersion: String?
    public let smuTelemetryState: SmuTelemetryState
    public let smuMetricsPlausible: Bool?
    public let amfiAnchorLoaded: Bool
    public let amfiAnchorInstallStatus: Int?
    public let amfiAnchorReady: Bool
    public let garnetAnchorLoaded: Bool
    public let amfiDisabledByBootArg: Bool
    public let recoveryCompleteNotice: String?
}

public enum DiagnosticsManager {
    public static func collectStatus(env: HostEnvironment) -> RuntimeStatusReport {
        let km = env.runner.run("/usr/bin/kmutil", ["showloaded", "--list-only"]).stdout
        let tokens = Set(km.split(whereSeparator: { $0.isWhitespace }).map(String.init))
        let drvLoaded = tokens.contains("org.garnet.driver")
        let accLoaded = tokens.contains("org.garnet.accel")
        let barCompatLoaded = tokens.contains("org.garnet.pcibarcompat")

        let drvIoreg = env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetDriver", "-d", "2"]).stdout
        var bar0MB: Int?
        var dispStatus: String?
        var matchMode: String?
        var matchUUID: String?
        var smuIfVer: String?
        var smuLayoutVerified: Bool?
        var smuValuesPlausible: Bool?

        for rawLine in drvIoreg.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.contains("\"ReBAR,BAR0,CurrentMB\""), let v = intValue(from: line) {
                bar0MB = v
            } else if line.contains("\"Garnet,DisplayPrimaryStatus\""), let s = stringValue(from: line) {
                dispStatus = s
            } else if line.contains("\"GarnetPCIBarCompat,MatchMode\""), let s = stringValue(from: line) {
                matchMode = s
            } else if line.contains("\"GarnetPCIBarCompat,IOPCIFamilyUUID\""), let s = stringValue(from: line) {
                matchUUID = s
            } else if line.contains("\"SMU,DriverIfVersion\""), let v = intValue(from: line), v > 0 {
                smuIfVer = String(format: "0x%x", v)
            } else if line.contains("\"SMU,MetricsLayoutVerified\""), let b = boolValue(from: line) {
                smuLayoutVerified = b
            } else if line.contains("\"SMU,MetricsValuesPlausible\""), let b = boolValue(from: line) {
                smuValuesPlausible = b
            }
        }

        if matchMode == nil {
            for svcClass in ["GarnetPCIBarCompat", "IOPCIDevice"] where matchMode == nil {
                let pciIoreg = env.runner.run("/usr/sbin/ioreg", ["-rc", svcClass, "-d", "1"]).stdout
                for rawLine in pciIoreg.components(separatedBy: .newlines) {
                    let line = rawLine.trimmingCharacters(in: .whitespaces)
                    if matchMode == nil && line.contains("\"GarnetPCIBarCompat,MatchMode\"") {
                        matchMode = stringValue(from: line)
                    }
                    if matchUUID == nil && line.contains("\"GarnetPCIBarCompat,IOPCIFamilyUUID\"") {
                        matchUUID = stringValue(from: line)
                    }
                }
            }
        }

        let primaryManifestURL = env.url(forSystemPath: GarnetPaths.manifestPath)
        let legacyManifestURL = env.url(forSystemPath: GarnetPaths.legacyManifestPath)
        let manifestURL = FileManager.default.fileExists(atPath: primaryManifestURL.path) ? primaryManifestURL : legacyManifestURL
        var manifestPath: String?
        var instVer: String?
        if FileManager.default.fileExists(atPath: manifestURL.path),
           let kv = try? PayloadVerifier.parseKVFile(at: manifestURL) {
            manifestPath = manifestURL.path
            instVer = kv["GARNET_VERSION"]
        }

        let vulkanURL = env.url(forSystemPath: GarnetPaths.vulkanICDPath)
        let vulkanInstalled = FileManager.default.fileExists(atPath: vulkanURL.path)

        let sip = env.readSIPStatus()
        var vramSummary: String?
        let primaryTest = env.url(forSystemPath: GarnetPaths.garnetTestPath)
        let legacyTest = env.url(forSystemPath: GarnetPaths.legacyGarnetTestPath)
        let garnettest = FileManager.default.isExecutableFile(atPath: primaryTest.path) ? primaryTest : legacyTest
        if env.effectiveUID == 0 && FileManager.default.isExecutableFile(atPath: garnettest.path) {
            let vr = env.runner.run(garnettest.path, ["accel", "vramstat"])
            if vr.exitCode == 0 && !vr.stdout.isEmpty {
                vramSummary = vr.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
            }
            let mr = env.runner.run(garnettest.path, ["metrics"])
            if mr.exitCode == 0 && !mr.stdout.isEmpty {
                let parsed = parseSmuMetricsOutput(mr.stdout)
                if let v = parsed.driverIfVersion { smuIfVer = v }
                if let lv = parsed.layoutVerified, smuLayoutVerified == nil || !lv { smuLayoutVerified = lv }
                if let vp = parsed.valuesPlausible, smuValuesPlausible == nil || !vp { smuValuesPlausible = vp }
            }
        }

        let smuState: SmuTelemetryState
        if smuLayoutVerified == false || smuValuesPlausible == false {
            smuState = .unverified
        } else if smuLayoutVerified == true {
            smuState = .verified
        } else {
            smuState = .unavailable
        }

        let denyCount = (try? DenylistManager.load(env: env).count) ?? 0
        let amfiStatus = env.readAMFIAnchorStatus(bootArgs: sip.bootArgs)
        let amfiAnchorKextLoaded = tokens.contains("org.b00t0x.AMFIAnchor") || amfiStatus.servicePresent

        var recoveryNotice: String?
        let recURL = env.url(forSystemPath: GarnetPaths.recoveryCompletePath)
        var recStat = stat()
        if lstat(recURL.path, &recStat) == 0 && (recStat.st_mode & S_IFMT) == S_IFREG,
           let rawRec = try? String(contentsOf: recURL, encoding: .utf8) {
            let trimmed = rawRec.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                recoveryNotice = trimmed
            }
        }

        return RuntimeStatusReport(
            driverLoaded: drvLoaded,
            accelLoaded: accLoaded,
            pciBarCompatLoaded: barCompatLoaded,
            bar0CurrentMB: bar0MB,
            displayPrimaryStatus: dispStatus,
            installedManifestPath: manifestPath,
            installedVersion: instVer,
            vulkanInstalled: vulkanInstalled,
            sipReadyForAuxKC: sip.readyForAuxKC,
            bootArgs: sip.bootArgs,
            iopciCompatMatchMode: matchMode,
            iopciCompatUUID: matchUUID,
            vramStatSummary: vramSummary,
            denylistCount: denyCount,
            smuDriverIfVersion: smuIfVer,
            smuTelemetryState: smuState,
            smuMetricsPlausible: smuValuesPlausible,
            amfiAnchorLoaded: amfiAnchorKextLoaded,
            amfiAnchorInstallStatus: amfiStatus.installStatus,
            amfiAnchorReady: amfiStatus.anchorReady,
            garnetAnchorLoaded: amfiStatus.garnetAnchorLoaded,
            amfiDisabledByBootArg: amfiStatus.amfiDisabledByBootArg,
            recoveryCompleteNotice: recoveryNotice
        )
    }

    public static func parseSmuMetricsOutput(_ stdout: String) -> SmuMetricsParseResult {
        var ifVer: String?
        var verified: Bool?
        var plausible: Bool?

        for rawLine in stdout.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.contains("UNVERIFIED") {
                verified = false
                if line.contains("range check: passed") {
                    plausible = true
                } else if line.contains("range check: FAILED") {
                    plausible = false
                }
            }
            if let r = line.range(of: "version 0x") {
                let tail = line[r.upperBound...]
                let hex = String(tail.prefix(while: { $0.isHexDigit }))
                if !hex.isEmpty {
                    ifVer = "0x\(hex.lowercased())"
                }
            }
        }
        if verified == nil && stdout.contains("clocks") {
            verified = true
            plausible = true
        }
        return SmuMetricsParseResult(driverIfVersion: ifVer, layoutVerified: verified, valuesPlausible: plausible)
    }

    public static func defaultDiagnosticsArchiveURL(in directory: URL? = nil, now: Date = Date()) -> URL {
        let fm = FileManager.default
        let targetDir: URL
        if let directory {
            targetDir = directory
        } else {
            let desktop = fm.homeDirectoryForCurrentUser.appendingPathComponent("Desktop")
            var isDir: ObjCBool = false
            if fm.fileExists(atPath: desktop.path, isDirectory: &isDir), isDir.boolValue {
                targetDir = desktop
            } else {
                targetDir = fm.homeDirectoryForCurrentUser
            }
        }
        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.dateFormat = "yyyyMMdd-HHmmss"
        return targetDir.appendingPathComponent("garnet-diagnostics-\(df.string(from: now)).tar.gz")
    }

    public static func sanitizeText(_ raw: String) -> String {
        var text = raw
        let home = NSHomeDirectory()
        if !home.isEmpty && home != "/" {
            text = text.replacingOccurrences(of: home, with: "/Users/<USER>")
        }
        let user = NSUserName()
        if !user.isEmpty && user != "root" {
            text = text.replacingOccurrences(of: "/Users/\(user)", with: "/Users/<USER>")
        }
        // Mask Mac serial / MLB / hardware UUID lines if present
        let outLines = text.components(separatedBy: .newlines).map { line -> String in
            let lower = line.trimmingCharacters(in: .whitespaces).lowercased()
            if lower.contains("ioplatformserialnumber")
                || lower.contains("ioplatformuuid")
                || lower.contains("serial-number")
                || lower.contains("hardware uuid:")
                || lower.contains("provisioning udid:")
                || lower.contains("serial number (system):")
                || lower.contains("\"mlb\"") {
                return "  [REDACTED HARDWARE IDENTIFIER]"
            }
            return line
        }
        return outLines.joined(separator: "\n")
    }

    private static func readRegularFileTail(at url: URL, maxBytes: Int) -> String? {
        var st = stat()
        guard lstat(url.path, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG else {
            return nil
        }
        guard let fh = FileHandle(forReadingAtPath: url.path) else {
            return nil
        }
        defer { try? fh.close() }
        let fileSize = st.st_size
        let truncated = fileSize > Int64(maxBytes)
        if truncated {
            try? fh.seek(toOffset: UInt64(fileSize - Int64(maxBytes)))
        }
        guard let data = try? fh.readToEnd() else {
            return nil
        }
        let text = String(decoding: data, as: UTF8.self)
        return truncated ? "[truncated to last \(maxBytes) bytes]\n\(text)" : text
    }

    private static func truncateUTF8Tail(_ text: String, maxBytes: Int) -> String {
        let utf8 = Array(text.utf8)
        guard utf8.count > maxBytes else { return text }
        var start = utf8.count - maxBytes
        // Advance past UTF-8 continuation bytes (10xxxxxx) so slicing stays valid.
        while start < utf8.count && (utf8[start] & 0xC0) == 0x80 {
            start += 1
        }
        let tail = String(decoding: utf8[start...], as: UTF8.self)
        return "[truncated to last \(maxBytes) bytes]\n\(tail)"
    }

    private static func formatCommandDump(title: String, result: CommandResult) -> String {
        var parts = ["=== \(title) (exit=\(result.exitCode)) ==="]
        if !result.stdout.isEmpty {
            parts.append(result.stdout.trimmingCharacters(in: .newlines))
        }
        if !result.stderr.isEmpty {
            parts.append("--- stderr ---\n\(result.stderr.trimmingCharacters(in: .newlines))")
        }
        return parts.joined(separator: "\n") + "\n"
    }

    private static func describeSystemPath(_ absPath: String, env: HostEnvironment) -> String {
        let url = env.url(forSystemPath: absPath)
        var st = stat()
        if lstat(url.path, &st) != 0 {
            return "\(absPath): missing"
        }
        let ftype: String
        switch st.st_mode & S_IFMT {
        case S_IFREG: ftype = "file"
        case S_IFDIR: ftype = "dir"
        case S_IFLNK: ftype = "symlink"
        default: ftype = "other"
        }
        let modeHex = String(format: "%04o", st.st_mode & 0o7777)
        if ftype == "file" && url.lastPathComponent == "profile" {
            let content = readRegularFileTail(at: url, maxBytes: 1024)?.trimmingCharacters(in: .newlines) ?? "<unreadable>"
            return "\(absPath): \(ftype) mode=\(modeHex) size=\(st.st_size) content=\(sanitizeText(content))"
        }
        return "\(absPath): \(ftype) mode=\(modeHex) size=\(st.st_size)"
    }

    public static func exportSanitizedDiagnostics(
        toArchive archiveURL: URL,
        env: HostEnvironment,
        payloadURL: URL? = nil,
        appActionLog: String? = nil
    ) throws -> URL {
        let fm = FileManager.default
        let stageDir = fm.temporaryDirectory.appendingPathComponent("garnet-diag-\(UUID().uuidString)")
        try fm.createDirectory(at: stageDir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: stageDir) }

        var summaryLines: [String] = []
        let utcFmt = ISO8601DateFormatter()
        summaryLines.append("timestamp_utc=\(utcFmt.string(from: Date()))")
        summaryLines.append("macos=\(env.readProductVersion()) (\(env.readBuildVersion()))")
        summaryLines.append("arch=\(env.readArch())")
        summaryLines.append("effective_uid=\(env.effectiveUID)")

        func writeText(_ name: String, _ text: String) {
            let ok = (try? sanitizeText(text).data(using: .utf8)?.write(to: stageDir.appendingPathComponent(name))) != nil
            summaryLines.append("artifact.\(name)=\(ok ? "written" : "write_failed")")
        }

        let status = collectStatus(env: env)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(status).write(to: stageDir.appendingPathComponent("status.json"))
        summaryLines.append("artifact.status.json=written")

        if let pURL = payloadURL {
            let preflight = PreflightChecker.run(env: env, payloadURL: pURL)
            try enc.encode(preflight).write(to: stageDir.appendingPathComponent("preflight.json"))
            summaryLines.append("artifact.preflight.json=written")
        } else {
            summaryLines.append("artifact.preflight.json=skipped(no_payload)")
        }

        let ioregRaw = env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetDriver", "-d", "3"]).stdout
        writeText("ioreg-garnet.txt", ioregRaw)

        let pciRaw = env.runner.run("/usr/sbin/ioreg", ["-rc", "IOPCIDevice", "-d", "1", "-l"]).stdout
        writeText("ioreg-pci.txt", pciRaw)

        let accelDispParts = [
            ("ioreg -rc GarnetMetalNub -d 2", env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetMetalNub", "-d", "2"])),
            ("ioreg -rc GarnetAccelerator -d 3", env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetAccelerator", "-d", "3"])),
            ("ioreg -rc AMDGarnetPrimaryFramebuffer -d 2", env.runner.run("/usr/sbin/ioreg", ["-rc", "AMDGarnetPrimaryFramebuffer", "-d", "2"])),
            ("ioreg -rc GarnetPCIBarCompat -d 1", env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetPCIBarCompat", "-d", "1"]))
        ].map { formatCommandDump(title: $0.0, result: $0.1) }
        writeText("ioreg-accel-display.txt", accelDispParts.joined(separator: "\n"))

        let amfiRaw = env.runner.run("/usr/sbin/ioreg", ["-r", "-c", "AMFIAnchor", "-l"]).stdout
        try sanitizeText(amfiRaw).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("ioreg-amfianchor.txt"))

        for (srcAbs, dstName) in [
            (GarnetPaths.recoveryLogPath, "recovery.log"),
            (GarnetPaths.recoveryCompletePath, "recovery-complete.txt")
        ] {
            let u = env.url(forSystemPath: srcAbs)
            if let text = try? String(contentsOf: u, encoding: .utf8) {
                try sanitizeText(text).data(using: .utf8)?.write(to: stageDir.appendingPathComponent(dstName))
            }
        }

        let kmLoaded = env.runner.run("/usr/bin/kmutil", ["showloaded"]).stdout
        writeText("kmutil-showloaded.txt", kmLoaded)

        let sysInfoParts = [
            ("sw_vers", env.runner.run("/usr/bin/sw_vers", [])),
            ("uname -a", env.runner.run("/usr/bin/uname", ["-a"])),
            ("sysctl kern.osversion", env.runner.run("/usr/sbin/sysctl", ["-n", "kern.osversion"])),
            ("sysctl kern.bootargs", env.runner.run("/usr/sbin/sysctl", ["-n", "kern.bootargs"])),
            ("sysctl kern.boottime", env.runner.run("/usr/sbin/sysctl", ["-n", "kern.boottime"])),
            ("csrutil status", env.runner.run("/usr/bin/csrutil", ["status"]))
        ].map { formatCommandDump(title: $0.0, result: $0.1) }
        writeText("system-info.txt", sysInfoParts.joined(separator: "\n"))

        let activationPaths = [
            GarnetPaths.profilePath,
            GarnetPaths.legacyProfilePath,
            "\(GarnetPaths.extensionsDir)/Garnet.kext",
            "\(GarnetPaths.extensionsDir)/GarnetAccel.kext",
            "\(GarnetPaths.gpuBundlesDir)/GarnetMetal.bundle",
            "\(GarnetPaths.launchDaemonsDir)/com.garnet.native.plist",
            "\(GarnetPaths.launchDaemonsDir)/com.garnet.native-translate.plist",
            GarnetPaths.nativeSupportDir,
            GarnetPaths.legacyNativeSupportDir,
            GarnetPaths.auxKCPath
        ]
        writeText("activation-state.txt", activationPaths.map { describeSystemPath($0, env: env) }.joined(separator: "\n") + "\n")

        let logFiles: [(name: String, path: String)] = [
            ("native-activation.stdout", "\(GarnetPaths.logsDir)/native-activation.stdout"),
            ("native-activation.stderr", "\(GarnetPaths.logsDir)/native-activation.stderr"),
            ("translate.stdout", "\(GarnetPaths.translateLogsDir)/translate.stdout"),
            ("translate.stderr", "\(GarnetPaths.translateLogsDir)/translate.stderr"),
            ("legacy-native-activation.stdout", "\(GarnetPaths.legacyLogsDir)/native-activation.stdout"),
            ("legacy-native-activation.stderr", "\(GarnetPaths.legacyLogsDir)/native-activation.stderr"),
            ("legacy-translate.stdout", "\(GarnetPaths.legacyTranslateLogsDir)/translate.stdout"),
            ("legacy-translate.stderr", "\(GarnetPaths.legacyTranslateLogsDir)/translate.stderr")
        ]
        let logTailLimit = 256 * 1024
        for lf in logFiles {
            let u = env.url(forSystemPath: lf.path)
            if let text = readRegularFileTail(at: u, maxBytes: logTailLimit) {
                writeText(lf.name, text)
            } else {
                summaryLines.append("artifact.\(lf.name)=absent_or_nonregular(\(lf.path))")
            }
        }

        let launchNative = env.runner.run("/bin/launchctl", ["print", "system/com.garnet.native"])
        writeText("launchctl-com.garnet.native.txt", formatCommandDump(title: "launchctl print system/com.garnet.native", result: launchNative))
        let launchXlate = env.runner.run("/bin/launchctl", ["print", "system/com.garnet.native-translate"])
        writeText("launchctl-com.garnet.native-translate.txt", formatCommandDump(title: "launchctl print system/com.garnet.native-translate", result: launchXlate))

        let displaysRes = env.runner.run("/usr/sbin/system_profiler", ["SPDisplaysDataType", "-timeout", "15"])
        writeText("displays.txt", formatCommandDump(title: "system_profiler SPDisplaysDataType -timeout 15", result: displaysRes))

        let psRes = env.runner.run("/bin/ps", ["-axo", "pid,ppid,uid,stat,etime,comm"])
        let psLines = psRes.stdout.components(separatedBy: .newlines)
        var filteredPS: [String] = []
        for (idx, line) in psLines.enumerated() where !line.isEmpty {
            if idx == 0 {
                filteredPS.append(line)
                continue
            }
            let lower = line.lowercased()
            if lower.contains("windowserver")
                || lower.contains("native-translate")
                || lower.contains("activate-native")
                || lower.contains("garnet")
                || lower.contains("loginwindow") {
                filteredPS.append(line)
            }
        }
        var psOut = "=== ps filtered (exit=\(psRes.exitCode)) ===\n" + filteredPS.joined(separator: "\n") + "\n"
        if !psRes.stderr.isEmpty {
            psOut += "--- stderr ---\n\(psRes.stderr)"
        }
        writeText("processes.txt", psOut)

        let logShow = env.runner.run("/usr/bin/log", ["show", "--last", "15m", "--predicate", "eventMessage CONTAINS 'garnet' OR eventMessage CONTAINS 'Garnet' OR eventMessage CONTAINS 'RDNA4'"]).stdout
        writeText("kernel-logs-15m.txt", logShow)

        let unifiedRes = env.runner.run("/usr/bin/log", [
            "show", "--last", "30m", "--style", "compact", "--info", "--predicate",
            "process == \"WindowServer\" OR process == \"kernelmanagerd\" OR eventMessage CONTAINS[c] \"garnet\" OR eventMessage CONTAINS[c] \"rdna4\" OR eventMessage CONTAINS \"IOAccelerator\""
        ])
        let unifiedBody = truncateUTF8Tail(formatCommandDump(title: "log show --last 30m", result: unifiedRes), maxBytes: 2 * 1024 * 1024)
        writeText("unified-logs-30m.txt", unifiedBody)

        let primaryTest = env.url(forSystemPath: GarnetPaths.garnetTestPath)
        let legacyTest = env.url(forSystemPath: GarnetPaths.legacyGarnetTestPath)
        let garnettest = fm.isExecutableFile(atPath: primaryTest.path) ? primaryTest : legacyTest
        if env.effectiveUID == 0 && fm.isExecutableFile(atPath: garnettest.path) {
            let gtCmds: [(String, [String])] = [
                ("garnettest-info.txt", ["info"]),
                ("garnettest-log.txt", ["log"]),
                ("garnettest-counters.txt", ["counters"]),
                ("garnettest-pipestat.txt", ["accel", "pipestat"]),
                ("garnettest-vramstat.txt", ["accel", "vramstat"])
            ]
            for (fname, gtArgs) in gtCmds {
                let res = env.runner.run(garnettest.path, gtArgs)
                writeText(fname, formatCommandDump(title: "garnettest \(gtArgs.joined(separator: " "))", result: res))
            }
        } else {
            summaryLines.append("artifact.garnettest=skipped(uid=\(env.effectiveUID),exists=\(fm.isExecutableFile(atPath: garnettest.path)))")
        }

        let diagManifestURL: URL = {
            let u = env.url(forSystemPath: GarnetPaths.manifestPath)
            return fm.fileExists(atPath: u.path) ? u : env.url(forSystemPath: GarnetPaths.legacyManifestPath)
        }()
        if let manifest = try? String(contentsOf: diagManifestURL, encoding: .utf8) {
            writeText("installed-MANIFEST.txt", manifest)
        } else {
            summaryLines.append("artifact.installed-MANIFEST.txt=absent")
        }
        if let denyURL = DenylistManager.resolvedReadURL(env: env),
           let deny = try? String(contentsOf: denyURL, encoding: .utf8) {
            writeText("app-denylist.txt", deny)
        } else {
            summaryLines.append("artifact.app-denylist.txt=absent")
        }

        if let appActionLog, !appActionLog.isEmpty {
            writeText("app-action-log.txt", appActionLog)
        }

        try sanitizeText(summaryLines.joined(separator: "\n") + "\n").data(using: .utf8)?.write(to: stageDir.appendingPathComponent("capture-summary.txt"))

        try fm.createDirectory(at: archiveURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        let tarRes = env.runner.run("/usr/bin/tar", ["-czf", archiveURL.path, "-C", stageDir.path, "."])
        if tarRes.exitCode != 0 {
            throw NSError(domain: "GarnetDiag", code: Int(tarRes.exitCode), userInfo: [NSLocalizedDescriptionKey: "tar failed: \(tarRes.stderr)"])
        }
        return archiveURL
    }

    private static func intValue(from line: String) -> Int? {
        guard let eq = line.range(of: "=") else { return nil }
        return Int(line[eq.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines))
    }

    private static func boolValue(from line: String) -> Bool? {
        guard let eq = line.range(of: "=") else { return nil }
        switch line[eq.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "yes", "true", "1": return true
        case "no", "false", "0": return false
        default: return nil
        }
    }

    private static func stringValue(from line: String) -> String? {
        guard let eq = line.range(of: "=") else { return nil }
        var rhs = String(line[eq.upperBound...]).trimmingCharacters(in: .whitespacesAndNewlines)
        if rhs.hasPrefix("\"") && rhs.hasSuffix("\"") && rhs.count >= 2 {
            rhs = String(rhs.dropFirst().dropLast())
        }
        return rhs
    }
}
