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
            smuMetricsPlausible: smuValuesPlausible
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
        // Mask Mac serial / MLB / IOPlatformUUID lines if present
        let outLines = text.components(separatedBy: .newlines).map { line -> String in
            let lower = line.lowercased()
            if lower.contains("ioplatformserialnumber") || lower.contains("ioplatformuuid") || lower.contains("serial-number") {
                return "  [REDACTED HARDWARE IDENTIFIER]"
            }
            return line
        }
        return outLines.joined(separator: "\n")
    }

    public static func exportSanitizedDiagnostics(
        toArchive archiveURL: URL,
        env: HostEnvironment,
        payloadURL: URL? = nil
    ) throws -> URL {
        let fm = FileManager.default
        let stageDir = fm.temporaryDirectory.appendingPathComponent("garnet-diag-\(UUID().uuidString)")
        try fm.createDirectory(at: stageDir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: stageDir) }

        let status = collectStatus(env: env)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(status).write(to: stageDir.appendingPathComponent("status.json"))

        if let pURL = payloadURL {
            let preflight = PreflightChecker.run(env: env, payloadURL: pURL)
            try enc.encode(preflight).write(to: stageDir.appendingPathComponent("preflight.json"))
        }

        let ioregRaw = env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetDriver", "-d", "3"]).stdout
        try sanitizeText(ioregRaw).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("ioreg-garnet.txt"))

        let pciRaw = env.runner.run("/usr/sbin/ioreg", ["-rc", "IOPCIDevice", "-d", "1", "-l"]).stdout
        try sanitizeText(pciRaw).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("ioreg-pci.txt"))

        let kmLoaded = env.runner.run("/usr/bin/kmutil", ["showloaded"]).stdout
        try sanitizeText(kmLoaded).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("kmutil-showloaded.txt"))

        let logShow = env.runner.run("/usr/bin/log", ["show", "--last", "15m", "--predicate", "eventMessage CONTAINS 'garnet' OR eventMessage CONTAINS 'Garnet' OR eventMessage CONTAINS 'RDNA4'"]).stdout
        try sanitizeText(logShow).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("kernel-logs-15m.txt"))

        let diagManifestURL: URL = {
            let u = env.url(forSystemPath: GarnetPaths.manifestPath)
            return FileManager.default.fileExists(atPath: u.path) ? u : env.url(forSystemPath: GarnetPaths.legacyManifestPath)
        }()
        if let manifest = try? String(contentsOf: diagManifestURL, encoding: .utf8) {
            try sanitizeText(manifest).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("installed-MANIFEST.txt"))
        }
        if let denyURL = DenylistManager.resolvedReadURL(env: env),
           let deny = try? String(contentsOf: denyURL, encoding: .utf8) {
            try sanitizeText(deny).data(using: .utf8)?.write(to: stageDir.appendingPathComponent("app-denylist.txt"))
        }

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
