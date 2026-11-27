import Foundation

public enum OpenCoreMode: String, Codable, CaseIterable, Sendable {
    case full = "full"
    case modeSmall = "small"

    public var displayName: String {
        switch self {
        case .full: return "Full-BAR (8/16 GiB)"
        case .modeSmall: return "256 MiB Small-BAR"
        }
    }
}

public enum OpenCoreAMFIMode: String, Codable, CaseIterable, Sendable {
    case anchor = "anchor"
    case legacy = "legacy"

    public var displayName: String {
        switch self {
        case .anchor: return "AMFIAnchor (AMFI Enabled)"
        case .legacy: return "Legacy (amfi=0x80)"
        }
    }
}

public enum FindingSeverity: String, Codable, Equatable, Sendable {
    case pass = "PASS"
    case warn = "WARN"
    case error = "ERROR"
}

public struct OpenCoreFinding: Codable, Equatable, Sendable {
    public let severity: FindingSeverity
    public let keyPath: String
    public let message: String
}

public struct OpenCoreReport: Codable, Equatable, Sendable {
    public let configPath: String
    public let mode: OpenCoreMode
    public let amfiMode: OpenCoreAMFIMode
    public let valid: Bool
    public let errorCount: Int
    public let warnCount: Int
    public let findings: [OpenCoreFinding]
    public let readOnlyVerified: Bool
}

public enum OpenCoreConfigureError: Error, CustomStringConvertible {
    case failed(String)

    public var description: String {
        switch self {
        case .failed(let message): return message
        }
    }
}

public struct OpenCoreConfigureResult: Codable, Equatable, Sendable {
    public let configPath: String
    public let backupPath: String
    public let baselineBackupPath: String?
    public let kextInstalledPath: String?
    public let recoverToolInstalledPath: String?
    public let anchorInjectorInstalledPath: String?
    public let amfiAnchorInstalledPath: String?
    public let mode: OpenCoreMode
    public let amfiMode: OpenCoreAMFIMode
    public let report: OpenCoreReport
}

public struct OpenCoreRestoreResult: Codable, Equatable, Sendable {
    public let configPath: String
    public let preRestoreBackupPath: String
    public let restoredFromBackupPath: String?
    public let stripOnly: Bool
    public let removedArtifacts: [String]
}

public enum OpenCoreValidator {
    public static let nvramBootGUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
    public static let garnetVendorGUID = "6E727447-6172-4E65-9452-65636F766572"
    public static let baselineBackupFilename = "config.plist.garnet-pre-garnet"
    public static let recoverToolFilename = "GarnetRecover.efi"
    public static let recoverToolDisplayName = "Garnet: Safe Boot & Recovery"

    private static let forbiddenSystemKexts: [(pattern: String, why: String)] = [
        ("Garnet.kext", "Must live in /Library/Extensions (AuxKC), never in OpenCore EFI/OC/Kexts/ (requires SystemKC IOGraphicsFamily)"),
        ("GarnetAccel.kext", "Must live in /Library/Extensions (AuxKC), never in OpenCore EFI/OC/Kexts/"),
        ("AMDRadeonRDNA4.kext", "Retired legacy Lilu plugin; must be disabled/removed"),
        ("RDNA4Bringup.kext", "Retired legacy bringup kext; must be disabled/removed"),
        ("RDNA4FB.kext", "Retired legacy framebuffer kext; must be disabled/removed"),
        ("RDNA4Accel.kext", "Retired legacy accel kext; must be disabled/removed"),
        ("Navi48Bringup.kext", "Retired Navi 48 bringup kext; must be disabled/removed")
    ]

    private static let baseRequiredBootArgs: [String] = [
        "garnet=1",
        "garnet-interrupts=1",
        "garnet-stage=17",
        "garnet-native=1",
        "garnet-metal=1",
        "garnet-metal-disp=1",
        "garnet-metal-ws=1",
        "garnet-multisession=1",
        "garnet-apps=1"
    ]

    public static func validate(
        configURL: URL,
        mode: OpenCoreMode,
        amfiMode: OpenCoreAMFIMode = .legacy
    ) throws -> OpenCoreReport {
        // Strictly read-only: open read bytes without ever writing or re-serializing the source file.
        let rawData = try Data(contentsOf: configURL, options: .mappedIfSafe)
        let plistAny = try PropertyListSerialization.propertyList(from: rawData, options: [], format: nil)
        guard let root = plistAny as? [String: Any] else {
            return OpenCoreReport(
                configPath: configURL.path,
                mode: mode,
                amfiMode: amfiMode,
                valid: false,
                errorCount: 1,
                warnCount: 0,
                findings: [OpenCoreFinding(severity: .error, keyPath: "/", message: "Root object of config.plist must be a dictionary")],
                readOnlyVerified: true
            )
        }

        var findings: [OpenCoreFinding] = []

        // 1. Kernel -> Add checks (placement contrast, GarnetPCIBarCompat, and AMFIAnchor ordering)
        let kernelAdd = ((root["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]]) ?? []
        var pciCompatEntry: [String: Any]?
        var liluIdx: Int?
        var amfiAnchorIdx: Int?
        var garnetInjectorIdx: Int?
        var garnetInjectorEntry: [String: Any]?
        var amfiPassIdx: Int?

        for (idx, entry) in kernelAdd.enumerated() {
            let bundlePath = (entry["BundlePath"] as? String) ?? ""
            let enabled = (entry["Enabled"] as? Bool) ?? false
            let baseName = (bundlePath as NSString).lastPathComponent

            if baseName == "GarnetPCIBarCompat.kext" {
                pciCompatEntry = entry
            }
            if enabled {
                if baseName == "Lilu.kext" && liluIdx == nil { liluIdx = idx }
                if baseName == "AMFIAnchor.kext" && amfiAnchorIdx == nil { amfiAnchorIdx = idx }
                if baseName == "GarnetAnchorInjector.kext" && garnetInjectorIdx == nil {
                    garnetInjectorIdx = idx
                    garnetInjectorEntry = entry
                }
                if baseName == "AMFIPass.kext" && amfiPassIdx == nil { amfiPassIdx = idx }
            }
            for rule in forbiddenSystemKexts {
                if (baseName == rule.pattern || baseName.hasPrefix("Navi48")) && enabled {
                    findings.append(OpenCoreFinding(
                        severity: .error,
                        keyPath: "Kernel.Add[\(bundlePath)]",
                        message: "\(baseName) is enabled in OpenCore: \(rule.why)"
                    ))
                }
            }
        }
        if !findings.contains(where: { $0.keyPath.hasPrefix("Kernel.Add[") }) {
            findings.append(OpenCoreFinding(
                severity: .pass,
                keyPath: "Kernel.Add",
                message: "No Garnet system kext or retired RDNA4/Navi48 kext enabled in OpenCore (Lilu/WEG unrelated entries preserved if needed)"
            ))
        }

        switch mode {
        case .full:
            if let compat = pciCompatEntry, ((compat["Enabled"] as? Bool) ?? false) {
                let execPath = (compat["ExecutablePath"] as? String) ?? ""
                let plistPath = (compat["PlistPath"] as? String) ?? ""
                if execPath == "Contents/MacOS/GarnetPCIBarCompat" && plistPath == "Contents/Info.plist" {
                    findings.append(OpenCoreFinding(
                        severity: .pass,
                        keyPath: "Kernel.Add[GarnetPCIBarCompat.kext]",
                        message: "GarnetPCIBarCompat.kext present and enabled for Full-BAR (8/16 GiB)"
                    ))
                } else {
                    findings.append(OpenCoreFinding(
                        severity: .error,
                        keyPath: "Kernel.Add[GarnetPCIBarCompat.kext]",
                        message: "GarnetPCIBarCompat.kext has invalid ExecutablePath ('\(execPath)') or PlistPath ('\(plistPath)')"
                    ))
                }
            } else {
                findings.append(OpenCoreFinding(
                    severity: .error,
                    keyPath: "Kernel.Add[GarnetPCIBarCompat.kext]",
                    message: "Full-BAR (8/16 GiB) mode requires GarnetPCIBarCompat.kext in EFI/OC/Kexts/ and Enabled = true"
                ))
            }
        case .modeSmall:
            if let compat = pciCompatEntry, ((compat["Enabled"] as? Bool) ?? false) {
                findings.append(OpenCoreFinding(
                    severity: .error,
                    keyPath: "Kernel.Add[GarnetPCIBarCompat.kext]",
                    message: "256 MiB Small-BAR mode requires GarnetPCIBarCompat.kext disabled (Enabled = false) or removed"
                ))
            } else {
                findings.append(OpenCoreFinding(
                    severity: .pass,
                    keyPath: "Kernel.Add[GarnetPCIBarCompat.kext]",
                    message: "GarnetPCIBarCompat.kext disabled or absent (matches 256 MiB Small-BAR mode)"
                ))
            }
        }

        // AMFIAnchor load-order checks when in anchor mode
        if amfiMode == .anchor {
            if liluIdx == nil {
                findings.append(OpenCoreFinding(
                    severity: .error,
                    keyPath: "Kernel.Add[Lilu.kext]",
                    message: "AMFIAnchor mode requires user-installed Lilu.kext enabled in Kernel.Add"
                ))
            }
            if amfiAnchorIdx == nil {
                findings.append(OpenCoreFinding(
                    severity: .error,
                    keyPath: "Kernel.Add[AMFIAnchor.kext]",
                    message: "AMFIAnchor mode requires AMFIAnchor.kext enabled in Kernel.Add after Lilu.kext"
                ))
            }
            if let inj = garnetInjectorEntry {
                let execPath = (inj["ExecutablePath"] as? String) ?? ""
                let plistPath = (inj["PlistPath"] as? String) ?? ""
                if !execPath.isEmpty || plistPath != "Contents/Info.plist" {
                    findings.append(OpenCoreFinding(
                        severity: .error,
                        keyPath: "Kernel.Add[GarnetAnchorInjector.kext]",
                        message: "Codeless GarnetAnchorInjector.kext must have empty ExecutablePath and PlistPath = Contents/Info.plist"
                    ))
                }
            } else {
                findings.append(OpenCoreFinding(
                    severity: .error,
                    keyPath: "Kernel.Add[GarnetAnchorInjector.kext]",
                    message: "AMFIAnchor mode requires GarnetAnchorInjector.kext enabled in Kernel.Add after AMFIAnchor.kext"
                ))
            }
            if let li = liluIdx, let ai = amfiAnchorIdx, let gi = garnetInjectorIdx {
                var orderOK = true
                if !(li < ai) {
                    orderOK = false
                    findings.append(OpenCoreFinding(
                        severity: .error,
                        keyPath: "Kernel.Add[AMFIAnchor.kext]",
                        message: "Load order error: AMFIAnchor.kext (index \(ai)) must load AFTER Lilu.kext (index \(li))"
                    ))
                }
                if !(ai < gi) {
                    orderOK = false
                    findings.append(OpenCoreFinding(
                        severity: .error,
                        keyPath: "Kernel.Add[GarnetAnchorInjector.kext]",
                        message: "Load order error: GarnetAnchorInjector.kext (index \(gi)) must load AFTER AMFIAnchor.kext (index \(ai))"
                    ))
                }
                if let pi = amfiPassIdx, !(gi < pi) {
                    orderOK = false
                    findings.append(OpenCoreFinding(
                        severity: .error,
                        keyPath: "Kernel.Add[AMFIPass.kext]",
                        message: "Load order error: AMFIPass.kext (index \(pi)) must load AFTER Lilu -> AMFIAnchor -> GarnetAnchorInjector"
                    ))
                }
                if orderOK {
                    findings.append(OpenCoreFinding(
                        severity: .pass,
                        keyPath: "Kernel.Add[AMFIAnchor]",
                        message: "AMFIAnchor order verified: Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext"
                    ))
                }
            }
        }

        // 2. NVRAM -> Add -> 7C436110... (boot-args + csr-active-config) & Delete
        let nvramAdd = (((root["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[nvramBootGUID] as? [String: Any]) ?? [:]
        let bootArgsStr = (nvramAdd["boot-args"] as? String) ?? ""
        let argTokens = bootArgsStr.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        let argSet = Set(argTokens)

        let expectedRequiredArgs = baseRequiredBootArgs + (amfiMode == .legacy ? ["amfi=0x80"] : [])
        let missingArgs = expectedRequiredArgs.filter { !argSet.contains($0) }
        if missingArgs.isEmpty {
            findings.append(OpenCoreFinding(
                severity: .pass,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args",
                message: "All required base Garnet boot-args present"
            ))
        } else {
            findings.append(OpenCoreFinding(
                severity: .error,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args",
                message: "Missing required boot-args: \(missingArgs.joined(separator: " "))"
            ))
        }

        if amfiMode == .anchor && argSet.contains("amfi=0x80") {
            findings.append(OpenCoreFinding(
                severity: .warn,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args[amfi]",
                message: "Remove amfi=0x80 in AMFIAnchor mode so macOS AMFI remains enabled"
            ))
        }

        if argSet.contains("-radvesa") {
            findings.append(OpenCoreFinding(
                severity: .error,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args",
                message: "Remove -radvesa from boot-args (disables GPU acceleration)"
            ))
        }
        let legacyTokens = argTokens.filter { $0.hasPrefix("rdna4-") || $0.hasPrefix("rdna4bringup") || $0.hasPrefix("navi48-") || $0.hasPrefix("navi48bringup") }
        if !legacyTokens.isEmpty {
            findings.append(OpenCoreFinding(
                severity: .error,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args",
                message: "Remove retired legacy tokens: \(legacyTokens.joined(separator: " "))"
            ))
        }

        if argSet.contains("garnet-apps-all=1") {
            findings.append(OpenCoreFinding(
                severity: .pass,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args[garnet-apps-all]",
                message: "garnet-apps-all=1 enabled (desktop apps receive Metal acceleration with 3-strike + appdeny guard)"
            ))
        } else {
            findings.append(OpenCoreFinding(
                severity: .warn,
                keyPath: "NVRAM.Add.\(nvramBootGUID).boot-args[garnet-apps-all]",
                message: "Recommend adding garnet-apps-all=1 for default desktop app Metal acceleration (otherwise only allowlisted apps get Metal)"
            ))
        }

        switch mode {
        case .full:
            if argSet.contains("garnet-full-bar=1") {
                findings.append(OpenCoreFinding(severity: .pass, keyPath: "NVRAM.boot-args[garnet-full-bar]", message: "garnet-full-bar=1 present"))
            } else {
                findings.append(OpenCoreFinding(severity: .error, keyPath: "NVRAM.boot-args[garnet-full-bar]", message: "Full-BAR (8/16 GiB) requires garnet-full-bar=1"))
            }
            if argSet.contains("npci=0x2000") || argSet.contains("npci=0x3000") {
                findings.append(OpenCoreFinding(severity: .error, keyPath: "NVRAM.boot-args[npci]", message: "Remove npci=0x2000/0x3000 in Full-BAR (8/16 GiB) mode (forces PCI resources below 4 GiB)"))
            }
            if argSet.contains("garnet-rebar-compat=0") {
                findings.append(OpenCoreFinding(severity: .error, keyPath: "NVRAM.boot-args[garnet-rebar-compat]", message: "Remove kill-switch garnet-rebar-compat=0 for normal Full-BAR (8/16 GiB) operation"))
            }
        case .modeSmall:
            if argSet.contains("garnet-full-bar=1") {
                findings.append(OpenCoreFinding(severity: .error, keyPath: "NVRAM.boot-args[garnet-full-bar]", message: "256 MiB Small-BAR mode must NOT carry garnet-full-bar=1"))
            } else {
                findings.append(OpenCoreFinding(severity: .pass, keyPath: "NVRAM.boot-args[garnet-full-bar]", message: "garnet-full-bar=1 absent (correct for Small-BAR)"))
            }
        }

        // csr-active-config == 03080000 (0x0803 LE) + NVRAM.Delete
        if let csrData = nvramAdd["csr-active-config"] as? Data, csrData == Data([0x03, 0x08, 0x00, 0x00]) {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "NVRAM.Add.\(nvramBootGUID).csr-active-config", message: "csr-active-config = 03080000 (0x0803)"))
        } else {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "NVRAM.Add.\(nvramBootGUID).csr-active-config", message: "csr-active-config must be Data <03080000> (0x0803)"))
        }

        let nvramDelete = (((root["NVRAM"] as? [String: Any])?["Delete"] as? [String: Any])?[nvramBootGUID] as? [String]) ?? []
        let delSet = Set(nvramDelete)
        if delSet.contains("boot-args") && delSet.contains("csr-active-config") {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "NVRAM.Delete.\(nvramBootGUID)", message: "NVRAM.Delete resets boot-args & csr-active-config"))
        } else {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "NVRAM.Delete.\(nvramBootGUID)", message: "NVRAM.Delete.\(nvramBootGUID) must contain both 'boot-args' and 'csr-active-config'"))
        }

        // 3. Misc -> Security -> SecureBootModel & Misc -> Tools (GarnetRecover.efi if present)
        let miscDict = (root["Misc"] as? [String: Any]) ?? [:]
        let secModel = ((miscDict["Security"] as? [String: Any])?["SecureBootModel"] as? String) ?? ""
        if secModel == "Disabled" {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "Misc.Security.SecureBootModel", message: "SecureBootModel = Disabled"))
        } else {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "Misc.Security.SecureBootModel", message: "SecureBootModel must be 'Disabled' (got '\(secModel)')"))
        }

        let miscTools = (miscDict["Tools"] as? [[String: Any]]) ?? []
        if let recTool = miscTools.first(where: { ((($0["Path"] as? String) ?? "") as NSString).lastPathComponent == recoverToolFilename }) {
            let enabled = (recTool["Enabled"] as? Bool) ?? false
            let path = (recTool["Path"] as? String) ?? ""
            if enabled && path == recoverToolFilename {
                findings.append(OpenCoreFinding(
                    severity: .pass,
                    keyPath: "Misc.Tools[\(recoverToolFilename)]",
                    message: "\(recoverToolFilename) enabled in OpenCore Picker"
                ))
            } else if enabled {
                findings.append(OpenCoreFinding(
                    severity: .error,
                    keyPath: "Misc.Tools[\(recoverToolFilename)]",
                    message: "\(recoverToolFilename) must use Path = '\(recoverToolFilename)' (got '\(path)')"
                ))
            }
        }

        // 4. Booter / UEFI / Kernel BAR Quirks
        let booterQuirks = ((root["Booter"] as? [String: Any])?["Quirks"] as? [String: Any]) ?? [:]
        let uefiQuirks = ((root["UEFI"] as? [String: Any])?["Quirks"] as? [String: Any]) ?? [:]
        let kernelQuirks = ((root["Kernel"] as? [String: Any])?["Quirks"] as? [String: Any]) ?? [:]

        let resizeApple = (booterQuirks["ResizeAppleGpuBars"] as? Int) ?? 0
        let resizeUefi = (uefiQuirks["ResizeGpuBars"] as? Int) ?? 0
        let incPciBar = (kernelQuirks["IncreasePciBarSize"] as? Bool) ?? false

        let wantResize = (mode == .full) ? -1 : 0
        if resizeApple == wantResize {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "Booter.Quirks.ResizeAppleGpuBars", message: "ResizeAppleGpuBars = \(wantResize)"))
        } else {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "Booter.Quirks.ResizeAppleGpuBars", message: "Expected ResizeAppleGpuBars = \(wantResize), found \(resizeApple)"))
        }

        if resizeUefi == wantResize {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "UEFI.Quirks.ResizeGpuBars", message: "ResizeGpuBars = \(wantResize)"))
        } else {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "UEFI.Quirks.ResizeGpuBars", message: "Expected ResizeGpuBars = \(wantResize), found \(resizeUefi)"))
        }

        if incPciBar {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "Kernel.Quirks.IncreasePciBarSize", message: "IncreasePciBarSize must be false (1 GiB cap bug if true)"))
        } else {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "Kernel.Quirks.IncreasePciBarSize", message: "IncreasePciBarSize = false"))
        }

        let errCnt = findings.filter { $0.severity == .error }.count
        let warnCnt = findings.filter { $0.severity == .warn }.count
        return OpenCoreReport(
            configPath: configURL.path,
            mode: mode,
            amfiMode: amfiMode,
            valid: errCnt == 0,
            errorCount: errCnt,
            warnCount: warnCnt,
            findings: findings,
            readOnlyVerified: true
        )
    }

    /// Resolve a user-selected EFI/OC directory or config.plist file without touching symlinks.
    public static func resolveConfigURL(from targetURL: URL) throws -> URL {
        let std = targetURL.standardizedFileURL
        try rejectSymlink(at: std)

        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: std.path, isDirectory: &isDir) else {
            throw OpenCoreConfigureError.failed("Path does not exist: \(std.path)")
        }

        let configURL: URL
        if isDir.boolValue {
            let candidates = [
                std.appendingPathComponent("config.plist"),
                std.appendingPathComponent("OC/config.plist"),
                std.appendingPathComponent("EFI/OC/config.plist")
            ]
            guard let found = candidates.first(where: { candidate in
                var subIsDir: ObjCBool = false
                return FileManager.default.fileExists(atPath: candidate.path, isDirectory: &subIsDir) && !subIsDir.boolValue
            }) else {
                throw OpenCoreConfigureError.failed("Could not find config.plist, OC/config.plist, or EFI/OC/config.plist under \(std.path)")
            }
            configURL = found
        } else {
            configURL = std
        }

        try rejectSymlink(at: configURL)
        return configURL
    }

    /// Back up config.plist (including one-time pre-Garnet baseline), deploy optional OpenCore artifacts,
    /// apply Garnet OpenCore settings, and verify.
    public static func configure(
        targetURL: URL,
        mode: OpenCoreMode,
        amfiMode: OpenCoreAMFIMode? = nil,
        payloadURL: URL?
    ) throws -> OpenCoreConfigureResult {
        let fm = FileManager.default
        let configURL = try resolveConfigURL(from: targetURL)
        let ocDir = configURL.deletingLastPathComponent()
        let kextsDir = ocDir.appendingPathComponent("Kexts")
        let toolsDir = ocDir.appendingPathComponent("Tools")
        try rejectSymlink(at: kextsDir)
        try rejectSymlink(at: toolsDir)

        let rawData = try Data(contentsOf: configURL)
        let plistAny = try PropertyListSerialization.propertyList(from: rawData, options: [], format: nil)
        guard var root = plistAny as? [String: Any] else {
            throw OpenCoreConfigureError.failed("Root object of \(configURL.path) must be a dictionary")
        }

        if mode == .full && payloadURL == nil {
            throw OpenCoreConfigureError.failed("Full-BAR (8/16 GiB) mode requires a valid payload to copy GarnetPCIBarCompat.kext")
        }

        let liluInEFI = kextsDir.appendingPathComponent("Lilu.kext")
        let amfiAnchorInEFI = kextsDir.appendingPathComponent("AMFIAnchor.kext")
        let injectorInEFI = kextsDir.appendingPathComponent("GarnetAnchorInjector.kext")
        let amfiAnchorInPayload = payloadURL?.appendingPathComponent("optional/AMFIAnchor.kext")
        let injectorInPayload = payloadURL?.appendingPathComponent("optional/GarnetAnchorInjector.kext")
        let recoverInPayload = payloadURL?.appendingPathComponent("optional/\(recoverToolFilename)")

        let hasLiluInEFI = isRegularDir(at: liluInEFI)
        let hasAmfiInPayload = amfiAnchorInPayload.map { isRegularDir(at: $0) } ?? false
        let hasAmfiInEFI = isRegularDir(at: amfiAnchorInEFI)
        let hasInjectorInPayload = injectorInPayload.map { isRegularDir(at: $0) } ?? false
        let hasInjectorInEFI = isRegularDir(at: injectorInEFI)

        let resolvedAmfiMode: OpenCoreAMFIMode
        if let explicit = amfiMode {
            resolvedAmfiMode = explicit
        } else if hasInjectorInPayload && hasLiluInEFI && (hasAmfiInPayload || hasAmfiInEFI) {
            resolvedAmfiMode = .anchor
        } else {
            resolvedAmfiMode = .legacy
        }

        // Fail closed before mutating config.plist or EFI if AMFIAnchor dependencies are missing.
        if resolvedAmfiMode == .anchor {
            guard hasLiluInEFI else {
                throw OpenCoreConfigureError.failed(
                    "AMFIAnchor mode requires user-installed Lilu.kext (1.7.2+) in \(kextsDir.path). Place Lilu.kext there or use --amfi legacy."
                )
            }
            guard hasAmfiInPayload || hasAmfiInEFI else {
                throw OpenCoreConfigureError.failed(
                    "AMFIAnchor mode requires AMFIAnchor.kext in \(kextsDir.path) (or bundled in payload). Place AMFIAnchor.kext there or use --amfi legacy."
                )
            }
            guard hasInjectorInPayload || hasInjectorInEFI else {
                throw OpenCoreConfigureError.failed(
                    "AMFIAnchor mode requires GarnetAnchorInjector.kext in payload (built with GARNET_SIGN_CA_DIR) or in \(kextsDir.path)."
                )
            }
        }

        let backupURL = try writeTimestampedBackup(rawData: rawData, nextTo: configURL, label: "garnet-backup")
        let baselineURL = ocDir.appendingPathComponent(baselineBackupFilename)
        try rejectSymlink(at: baselineURL)
        var baselineRecordedPath: String?
        if fm.fileExists(atPath: baselineURL.path) {
            baselineRecordedPath = baselineURL.path
        } else if !looksAlreadyGarnetConfigured(root) {
            try rawData.write(to: baselineURL, options: .atomic)
            baselineRecordedPath = baselineURL.path
        }

        var kextInstalledPath: String?
        if mode == .full, let payloadURL {
            let dst = try SystemDiskInstaller.exportPCIBarCompat(fromPayload: payloadURL, toDestination: kextsDir)
            kextInstalledPath = dst.path
        }

        var recoverInstalledPath: String?
        if let payloadURL, let recSrc = recoverInPayload, fm.fileExists(atPath: recSrc.path) {
            let dst = try SystemDiskInstaller.exportRecoverEFI(fromPayload: payloadURL, toDestination: toolsDir)
            recoverInstalledPath = dst.path
            applyRecoveryToolEntry(to: &root)
        }

        var anchorInjectorInstalledPath: String?
        var amfiAnchorInstalledPath: String?
        if resolvedAmfiMode == .anchor {
            if let payloadURL, hasInjectorInPayload {
                let dst = try SystemDiskInstaller.exportAnchorInjector(fromPayload: payloadURL, toDestination: kextsDir)
                anchorInjectorInstalledPath = dst.path
            } else {
                anchorInjectorInstalledPath = injectorInEFI.path
            }
            if let payloadURL, hasAmfiInPayload {
                if let dst = try SystemDiskInstaller.exportAMFIAnchor(fromPayload: payloadURL, toDestination: kextsDir) {
                    amfiAnchorInstalledPath = dst.path
                }
            } else {
                amfiAnchorInstalledPath = amfiAnchorInEFI.path
            }
        }

        applyKernelSettings(to: &root, mode: mode, amfiMode: resolvedAmfiMode)
        applyQuirks(to: &root, mode: mode)
        applySecurityAndNVRAM(to: &root, mode: mode, amfiMode: resolvedAmfiMode)

        let newData = try PropertyListSerialization.data(fromPropertyList: root, format: .xml, options: 0)
        try newData.write(to: configURL, options: .atomic)

        let report = try validate(configURL: configURL, mode: mode, amfiMode: resolvedAmfiMode)
        guard report.valid else {
            try? rawData.write(to: configURL, options: .atomic)
            let issues = report.findings.filter { $0.severity == .error }.map { "\($0.keyPath): \($0.message)" }.joined(separator: "; ")
            throw OpenCoreConfigureError.failed("Post-write validation failed (restored original config.plist): \(issues)")
        }

        return OpenCoreConfigureResult(
            configPath: configURL.path,
            backupPath: backupURL.path,
            baselineBackupPath: baselineRecordedPath,
            kextInstalledPath: kextInstalledPath,
            recoverToolInstalledPath: recoverInstalledPath,
            anchorInjectorInstalledPath: anchorInjectorInstalledPath,
            amfiAnchorInstalledPath: amfiAnchorInstalledPath,
            mode: mode,
            amfiMode: resolvedAmfiMode,
            report: report
        )
    }

    /// Restore a manually mounted OpenCore config.plist from the recorded pre-Garnet baseline backup
    /// (or explicit backup), or strip Garnet-managed entries in-place. Never removes Lilu.kext or AMFIAnchor.kext.
    public static func restore(
        targetURL: URL,
        backupURL: URL? = nil,
        stripOnly: Bool = false
    ) throws -> OpenCoreRestoreResult {
        let fm = FileManager.default
        let configURL = try resolveConfigURL(from: targetURL)
        let ocDir = configURL.deletingLastPathComponent()
        let kextsDir = ocDir.appendingPathComponent("Kexts")
        let toolsDir = ocDir.appendingPathComponent("Tools")
        try rejectSymlink(at: kextsDir)
        try rejectSymlink(at: toolsDir)

        let currentData = try Data(contentsOf: configURL)
        let preRestoreBackup = try writeTimestampedBackup(rawData: currentData, nextTo: configURL, label: "garnet-before-restore")

        var restoredFrom: String?
        var effectiveStripOnly = stripOnly

        if !stripOnly {
            let candidate: URL?
            if let explicit = backupURL {
                try rejectSymlink(at: explicit)
                guard fm.fileExists(atPath: explicit.path) else {
                    throw OpenCoreConfigureError.failed("Specified backup does not exist: \(explicit.path)")
                }
                candidate = explicit
            } else {
                let baseline = ocDir.appendingPathComponent(baselineBackupFilename)
                try rejectSymlink(at: baseline)
                candidate = fm.fileExists(atPath: baseline.path) ? baseline : nil
            }

            if let srcBackup = candidate {
                let bakData = try Data(contentsOf: srcBackup)
                let obj = try PropertyListSerialization.propertyList(from: bakData, options: [], format: nil)
                guard obj is [String: Any] else {
                    throw OpenCoreConfigureError.failed("Backup plist is not a valid dictionary: \(srcBackup.path)")
                }
                try bakData.write(to: configURL, options: .atomic)
                restoredFrom = srcBackup.path
            } else {
                effectiveStripOnly = true
            }
        }

        if effectiveStripOnly {
            let plistAny = try PropertyListSerialization.propertyList(from: currentData, options: [], format: nil)
            guard var root = plistAny as? [String: Any] else {
                throw OpenCoreConfigureError.failed("Root object of \(configURL.path) must be a dictionary")
            }
            stripGarnetEntries(from: &root)
            let strippedData = try PropertyListSerialization.data(fromPropertyList: root, format: .xml, options: 0)
            try strippedData.write(to: configURL, options: .atomic)
        }

        // Remove only verified Garnet-owned artifacts from EFI; never touch Lilu.kext or AMFIAnchor.kext.
        var removed: [String] = []
        let pciCompatURL = kextsDir.appendingPathComponent("GarnetPCIBarCompat.kext")
        if try removeVerifiedKextIfPresent(at: pciCompatURL, expectedBundleID: "org.garnet.pcibarcompat") {
            removed.append(pciCompatURL.path)
        }
        let anchorInjURL = kextsDir.appendingPathComponent("GarnetAnchorInjector.kext")
        if try removeVerifiedKextIfPresent(at: anchorInjURL, expectedBundleID: "org.garnet.anchor-injector") {
            removed.append(anchorInjURL.path)
        }
        let recoverToolURL = toolsDir.appendingPathComponent(recoverToolFilename)
        try rejectSymlink(at: recoverToolURL)
        if fm.fileExists(atPath: recoverToolURL.path) {
            if (try? PayloadVerifier.verifyPE32PlusEFI(at: recoverToolURL)) != nil {
                try fm.removeItem(at: recoverToolURL)
                removed.append(recoverToolURL.path)
            }
        }

        return OpenCoreRestoreResult(
            configPath: configURL.path,
            preRestoreBackupPath: preRestoreBackup.path,
            restoredFromBackupPath: restoredFrom,
            stripOnly: effectiveStripOnly,
            removedArtifacts: removed
        )
    }

    private static func isRegularDir(at url: URL) -> Bool {
        var st = stat()
        return lstat(url.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
    }

    private static func looksAlreadyGarnetConfigured(_ root: [String: Any]) -> Bool {
        let nvramAdd = (((root["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[nvramBootGUID] as? [String: Any]) ?? [:]
        let args = Set(((nvramAdd["boot-args"] as? String) ?? "").split(whereSeparator: { $0.isWhitespace }).map(String.init))
        return args.contains("garnet=1") && args.contains("garnet-stage=17")
    }

    private static func removeVerifiedKextIfPresent(at kextURL: URL, expectedBundleID: String) throws -> Bool {
        try rejectSymlink(at: kextURL)
        let fm = FileManager.default
        guard isRegularDir(at: kextURL) else { return false }
        let plistURL = kextURL.appendingPathComponent("Contents/Info.plist")
        try rejectSymlink(at: plistURL)
        guard let data = try? Data(contentsOf: plistURL),
              let dict = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any],
              (dict["CFBundleIdentifier"] as? String) == expectedBundleID else {
            return false
        }
        try fm.removeItem(at: kextURL)
        return true
    }

    private static func rejectSymlink(at url: URL) throws {
        var st = stat()
        if lstat(url.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK {
            throw OpenCoreConfigureError.failed("Refusing symbolic link path: \(url.path)")
        }
    }

    private static func writeTimestampedBackup(rawData: Data, nextTo configURL: URL, label: String) throws -> URL {
        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.dateFormat = "yyyyMMdd-HHmmss"
        let baseName = "\(configURL.lastPathComponent).\(label)-\(df.string(from: Date()))"
        let dir = configURL.deletingLastPathComponent()

        var backupURL = dir.appendingPathComponent(baseName)
        var suffix = 1
        while FileManager.default.fileExists(atPath: backupURL.path) {
            backupURL = dir.appendingPathComponent("\(baseName)-\(suffix)")
            suffix += 1
        }
        try rawData.write(to: backupURL, options: .atomic)
        return backupURL
    }

    private static func applyRecoveryToolEntry(to root: inout [String: Any]) {
        var misc = (root["Misc"] as? [String: Any]) ?? [:]
        var tools = (misc["Tools"] as? [[String: Any]]) ?? []
        let entry: [String: Any] = [
            "Arguments": "",
            "Auxiliary": false,
            "Comment": "Garnet Safe Boot & Recovery Tool (zero ESP writes)",
            "Enabled": true,
            "Flavour": "Auto",
            "FullNvramAccess": true,
            "Name": recoverToolDisplayName,
            "Path": recoverToolFilename,
            "RealPath": false,
            "TextMode": true
        ]
        var updated = false
        for idx in tools.indices {
            let p = ((tools[idx]["Path"] as? String) ?? "") as NSString
            if p.lastPathComponent == recoverToolFilename {
                if !updated {
                    tools[idx] = entry
                    updated = true
                } else {
                    tools[idx]["Enabled"] = false
                }
            }
        }
        if !updated {
            tools.append(entry)
        }
        misc["Tools"] = tools
        root["Misc"] = misc
    }

    private static func applyKernelSettings(
        to root: inout [String: Any],
        mode: OpenCoreMode,
        amfiMode: OpenCoreAMFIMode
    ) {
        var kernel = (root["Kernel"] as? [String: Any]) ?? [:]
        var kernelAdd = (kernel["Add"] as? [[String: Any]]) ?? []
        var foundCompat = false

        for idx in kernelAdd.indices {
            let bundlePath = (kernelAdd[idx]["BundlePath"] as? String) ?? ""
            let baseName = (bundlePath as NSString).lastPathComponent

            if forbiddenSystemKexts.contains(where: { baseName == $0.pattern }) || baseName.hasPrefix("Navi48") {
                kernelAdd[idx]["Enabled"] = false
            }

            if baseName == "GarnetPCIBarCompat.kext" {
                switch mode {
                case .full:
                    if !foundCompat {
                        kernelAdd[idx]["BundlePath"] = "GarnetPCIBarCompat.kext"
                        kernelAdd[idx]["Enabled"] = true
                        kernelAdd[idx]["ExecutablePath"] = "Contents/MacOS/GarnetPCIBarCompat"
                        kernelAdd[idx]["PlistPath"] = "Contents/Info.plist"
                        if (kernelAdd[idx]["MinKernel"] as? String ?? "").isEmpty {
                            kernelAdd[idx]["MinKernel"] = "25.0.0"
                        }
                        foundCompat = true
                    } else {
                        kernelAdd[idx]["Enabled"] = false
                    }
                case .modeSmall:
                    kernelAdd[idx]["Enabled"] = false
                }
            }

            if amfiMode == .legacy && baseName == "GarnetAnchorInjector.kext" {
                kernelAdd[idx]["Enabled"] = false
            }
        }

        if mode == .full && !foundCompat {
            kernelAdd.append([
                "Arch": "x86_64",
                "BundlePath": "GarnetPCIBarCompat.kext",
                "Comment": "Garnet Full-BAR (8/16 GiB) compatibility",
                "Enabled": true,
                "ExecutablePath": "Contents/MacOS/GarnetPCIBarCompat",
                "MaxKernel": "",
                "MinKernel": "25.0.0",
                "PlistPath": "Contents/Info.plist"
            ])
        }

        if amfiMode == .anchor {
            kernelAdd = arrangeAMFIAnchorEntries(in: kernelAdd)
        }

        var quirks = (kernel["Quirks"] as? [String: Any]) ?? [:]
        quirks["IncreasePciBarSize"] = false
        kernel["Add"] = kernelAdd
        kernel["Quirks"] = quirks
        root["Kernel"] = kernel
    }

    /// Guarantee enabled order: Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext (all before AMFIPass.kext).
    private static func arrangeAMFIAnchorEntries(in entries: [[String: Any]]) -> [[String: Any]] {
        var result: [[String: Any]] = []
        var liluEntry: [String: Any]?
        var amfiEntry: [String: Any]?
        var injEntry: [String: Any]?

        for entry in entries {
            let base = (((entry["BundlePath"] as? String) ?? "") as NSString).lastPathComponent
            if base == "Lilu.kext" && liluEntry == nil {
                var e = entry
                e["Enabled"] = true
                liluEntry = e
            } else if base == "AMFIAnchor.kext" && amfiEntry == nil {
                var e = entry
                e["BundlePath"] = "AMFIAnchor.kext"
                e["Enabled"] = true
                e["ExecutablePath"] = "Contents/MacOS/AMFIAnchor"
                e["PlistPath"] = "Contents/Info.plist"
                e["MinKernel"] = "20.0.0"
                e["MaxKernel"] = "25.99.99"
                amfiEntry = e
            } else if base == "GarnetAnchorInjector.kext" && injEntry == nil {
                var e = entry
                e["BundlePath"] = "GarnetAnchorInjector.kext"
                e["Enabled"] = true
                e["ExecutablePath"] = ""
                e["PlistPath"] = "Contents/Info.plist"
                e["MinKernel"] = "25.0.0"
                e["MaxKernel"] = "25.99.99"
                injEntry = e
            } else if base == "AMFIAnchor.kext" || base == "GarnetAnchorInjector.kext" {
                continue
            } else {
                result.append(entry)
            }
        }

        let finalLilu: [String: Any] = liluEntry ?? [
            "Arch": "x86_64",
            "BundlePath": "Lilu.kext",
            "Comment": "Lilu (required for AMFIAnchor)",
            "Enabled": true,
            "ExecutablePath": "Contents/MacOS/Lilu",
            "MaxKernel": "",
            "MinKernel": "",
            "PlistPath": "Contents/Info.plist"
        ]
        let finalAmfi: [String: Any] = amfiEntry ?? [
            "Arch": "x86_64",
            "BundlePath": "AMFIAnchor.kext",
            "Comment": "AMFIAnchor trust-anchor engine",
            "Enabled": true,
            "ExecutablePath": "Contents/MacOS/AMFIAnchor",
            "MaxKernel": "25.99.99",
            "MinKernel": "20.0.0",
            "PlistPath": "Contents/Info.plist"
        ]
        let finalInj: [String: Any] = injEntry ?? [
            "Arch": "x86_64",
            "BundlePath": "GarnetAnchorInjector.kext",
            "Comment": "Garnet Root CA public trust anchor (codeless)",
            "Enabled": true,
            "ExecutablePath": "",
            "MaxKernel": "25.99.99",
            "MinKernel": "25.0.0",
            "PlistPath": "Contents/Info.plist"
        ]

        // Insert Lilu -> AMFIAnchor -> GarnetAnchorInjector at index 0 (guaranteed before AMFIPass.kext and other plugins).
        return [finalLilu, finalAmfi, finalInj] + result
    }

    private static func stripGarnetEntries(from root: inout [String: Any]) {
        if var kernel = root["Kernel"] as? [String: Any],
           var kernelAdd = kernel["Add"] as? [[String: Any]] {
            kernelAdd.removeAll { entry in
                let base = (((entry["BundlePath"] as? String) ?? "") as NSString).lastPathComponent
                return base == "GarnetPCIBarCompat.kext" || base == "GarnetAnchorInjector.kext"
            }
            kernel["Add"] = kernelAdd
            root["Kernel"] = kernel
        }

        if var misc = root["Misc"] as? [String: Any],
           var tools = misc["Tools"] as? [[String: Any]] {
            tools.removeAll { entry in
                (((entry["Path"] as? String) ?? "") as NSString).lastPathComponent == recoverToolFilename
            }
            misc["Tools"] = tools
            root["Misc"] = misc
        }

        if var nvram = root["NVRAM"] as? [String: Any],
           var nvramAdd = nvram["Add"] as? [String: Any],
           var bootDict = nvramAdd[nvramBootGUID] as? [String: Any],
           let existing = bootDict["boot-args"] as? String {
            let stripPrefixes = [
                "garnet=", "garnet-interrupts=", "garnet-stage=", "garnet-native=",
                "garnet-metal=", "garnet-metal-disp=", "garnet-metal-ws=", "garnet-multisession=",
                "garnet-apps=", "garnet-apps-all=", "garnet-full-bar=", "garnet-aux=",
                "garnet-rebar-compat=", "rdna4-", "rdna4bringup", "navi48-", "navi48bringup"
            ]
            let kept = existing.split(whereSeparator: { $0.isWhitespace }).map(String.init).filter { tok in
                !stripPrefixes.contains(where: { tok.hasPrefix($0) })
            }
            bootDict["boot-args"] = kept.joined(separator: " ")
            nvramAdd[nvramBootGUID] = bootDict
            nvram["Add"] = nvramAdd
            root["NVRAM"] = nvram
        }
    }

    private static func applyQuirks(to root: inout [String: Any], mode: OpenCoreMode) {
        let wantResize = (mode == .full) ? -1 : 0

        var booter = (root["Booter"] as? [String: Any]) ?? [:]
        var booterQuirks = (booter["Quirks"] as? [String: Any]) ?? [:]
        booterQuirks["ResizeAppleGpuBars"] = wantResize
        booter["Quirks"] = booterQuirks
        root["Booter"] = booter

        var uefi = (root["UEFI"] as? [String: Any]) ?? [:]
        var uefiQuirks = (uefi["Quirks"] as? [String: Any]) ?? [:]
        uefiQuirks["ResizeGpuBars"] = wantResize
        uefi["Quirks"] = uefiQuirks
        root["UEFI"] = uefi
    }

    private static func applySecurityAndNVRAM(
        to root: inout [String: Any],
        mode: OpenCoreMode,
        amfiMode: OpenCoreAMFIMode
    ) {
        var misc = (root["Misc"] as? [String: Any]) ?? [:]
        var security = (misc["Security"] as? [String: Any]) ?? [:]
        security["SecureBootModel"] = "Disabled"
        misc["Security"] = security
        root["Misc"] = misc

        var nvram = (root["NVRAM"] as? [String: Any]) ?? [:]
        var nvramAdd = (nvram["Add"] as? [String: Any]) ?? [:]
        var bootDict = (nvramAdd[nvramBootGUID] as? [String: Any]) ?? [:]

        bootDict["csr-active-config"] = Data([0x03, 0x08, 0x00, 0x00])
        let existingArgs = (bootDict["boot-args"] as? String) ?? ""
        bootDict["boot-args"] = updatedBootArgs(from: existingArgs, mode: mode, amfiMode: amfiMode)
        nvramAdd[nvramBootGUID] = bootDict
        nvram["Add"] = nvramAdd

        var nvramDelete = (nvram["Delete"] as? [String: Any]) ?? [:]
        var deleteKeys = (nvramDelete[nvramBootGUID] as? [String]) ?? []
        for key in ["boot-args", "csr-active-config"] where !deleteKeys.contains(key) {
            deleteKeys.append(key)
        }
        nvramDelete[nvramBootGUID] = deleteKeys
        nvram["Delete"] = nvramDelete
        root["NVRAM"] = nvram
    }

    private static func updatedBootArgs(
        from existing: String,
        mode: OpenCoreMode,
        amfiMode: OpenCoreAMFIMode
    ) -> String {
        let managedPrefixes = [
            "garnet=", "garnet-interrupts=", "garnet-stage=", "garnet-native=",
            "garnet-metal=", "garnet-metal-disp=", "garnet-metal-ws=", "garnet-multisession=",
            "garnet-apps=", "garnet-apps-all=", "garnet-full-bar=", "garnet-aux=",
            "garnet-rebar-compat=", "amfi=", "rdna4-", "rdna4bringup", "navi48-", "navi48bringup"
        ]
        var kept: [String] = []
        for raw in existing.split(whereSeparator: { $0.isWhitespace }) {
            let tok = String(raw)
            if tok == "-radvesa" { continue }
            if mode == .full && (tok == "npci=0x2000" || tok == "npci=0x3000") { continue }
            if managedPrefixes.contains(where: { tok.hasPrefix($0) }) { continue }
            kept.append(tok)
        }

        kept.append(contentsOf: baseRequiredBootArgs)
        kept.append("garnet-apps-all=1")
        if amfiMode == .legacy {
            kept.append("amfi=0x80")
        }
        switch mode {
        case .full:
            kept.append("garnet-full-bar=1")
        case .modeSmall:
            if !kept.contains("npci=0x2000") && !kept.contains("npci=0x3000") {
                kept.append("npci=0x2000")
            }
        }
        return kept.joined(separator: " ")
    }

    public static func sampleConfigSnippet(mode: OpenCoreMode, amfiMode: OpenCoreAMFIMode = .legacy) -> String {
        let amfiArg = (amfiMode == .legacy) ? " amfi=0x80" : ""
        let anchorNote: String
        if amfiMode == .anchor {
            anchorNote = """
               - AMFIAnchor mode (no amfi=0x80): place in EFI/OC/Kexts/ and Enable in exact order (before AMFIPass.kext if used):
                 1) Lilu.kext (1.7.2+, user-supplied)
                 2) AMFIAnchor.kext (MinKernel=20.0.0 MaxKernel=25.99.99)
                 3) GarnetAnchorInjector.kext (codeless: ExecutablePath="", MinKernel=25.0.0 MaxKernel=25.99.99)
            """
        } else {
            anchorNote = "   - Legacy AMFI mode: uses amfi=0x80 in boot-args (no Lilu/AMFIAnchor required)."
        }

        switch mode {
        case .full:
            return """
            # Full-BAR (8/16 GiB) OpenCore Checklist [\(amfiMode.displayName)]
            1. Copy `GarnetPCIBarCompat.kext` to `EFI/OC/Kexts/` and add & Enable in `Kernel -> Add`
               (Place AFTER Lilu/WEG/VirtualSMC; set MaxKernel="" MinKernel="25.0.0").
            \(anchorNote)
            2. NEVER put Garnet.kext or GarnetAccel.kext inside `EFI/OC/Kexts/`.
            3. Optional Recovery Tool: copy `GarnetRecover.efi` to `EFI/OC/Tools/` and enable in `Misc -> Tools` (`FullNvramAccess = true`, `TextMode = true`).
            4. `Booter -> Quirks -> ResizeAppleGpuBars` = -1
               `UEFI -> Quirks -> ResizeGpuBars` = -1
               `Kernel -> Quirks -> IncreasePciBarSize` = False
            5. `Misc -> Security -> SecureBootModel` = "Disabled"
            6. `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`:
               csr-active-config = <03080000>  (0x0803)
               boot-args = -v keepsyms=1 garnet=1 garnet-interrupts=1 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1\(amfiArg) garnet-full-bar=1
               (DO NOT include npci=0x2000 or -radvesa)
            7. `NVRAM -> Delete -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`: add `boot-args` and `csr-active-config`.
            """
        case .modeSmall:
            return """
            # 256 MiB Small-BAR OpenCore Checklist [\(amfiMode.displayName)]
            1. Remove or Disable `GarnetPCIBarCompat.kext` in `Kernel -> Add`.
            \(anchorNote)
            2. Optional Recovery Tool: copy `GarnetRecover.efi` to `EFI/OC/Tools/` and enable in `Misc -> Tools` (`FullNvramAccess = true`, `TextMode = true`).
            3. `Booter -> Quirks -> ResizeAppleGpuBars` = 0
               `UEFI -> Quirks -> ResizeGpuBars` = 0
               `Kernel -> Quirks -> IncreasePciBarSize` = False
            4. `Misc -> Security -> SecureBootModel` = "Disabled"
            5. `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`:
               csr-active-config = <03080000>  (0x0803)
               boot-args = -v keepsyms=1 npci=0x2000 garnet=1 garnet-interrupts=1 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1\(amfiArg)
               (DO NOT include garnet-full-bar=1 or -radvesa)
            6. `NVRAM -> Delete -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`: add `boot-args` and `csr-active-config`.
            """
        }
    }

    public static func emergencyKillSwitchesText() -> String {
        """
        Emergency OpenCore Recovery & boot-arg Kill-Switches:
          - OpenCore Picker -> `Garnet: Safe Boot & Recovery` (`GarnetRecover.efi`, zero ESP disk writes):
              [1] Safe Boot Once (`garnet=0 garnet-aux=0 garnet-rebar-compat=0`, keeps driver installed)
              [2] Safe Boot & Uninstall Garnet (uninstalls system-disk files on next macOS boot, stays on safe desktop; restore config.plist via `restore-oc`)
          - `garnet=0`              Disables Garnet.kext primary driver & GPU init completely (boots on VESA/fallback display).
          - `garnet-aux=0`          Disables GarnetAccel.kext / GarnetFramebuffer while keeping bringup diagnostics accessible.
          - `garnet-rebar-compat=0` Disables GarnetPCIBarCompat.kext Full-BAR hook without removing it from EFI/OC/Kexts/.
          - `garnet-apps-all=0`     Disables global desktop app Metal access while preserving core compositor & allowlisted tools.
          - `garnet-irq-wait=0`     Reverts kernel fence waits to legacy polling without disabling MSI interrupts.
          - `garnet-vbl-irq=0`      Reverts primary framebuffer VBLANK delivery to software timer without disabling MSI.
          - `garnet-dpms=0`         Disables hardware DisplayPort DPMS sleep/wake stream blanking and PHY power-down.
          - `garnet-gfx-cg=0`       Disables GFX12 CGCG/CGLS/MGCG/FGCG clock gating (leaves hardware reset defaults).
          - `garnet-det0=0`         Disables single-pipe DCN41 DET0 CRB expansion (keeps UEFI 192 KiB DET0).
          - `garnet-uclk-pin=1`     Forces GDDR6 UCLK pinned at top DPM clock (disables DET0-guarded UCLK unpinning).
          - `garnet-webkitgpu=0`    Disables Garnet.kext's version-independent _mac_file_check_mmap Safari WebKit.GPU hook.
          - `garnet-hpd-recover=0`  Disables automatic DCN 4.0.1 DisplayPort HPD & link-loss recovery retraining.
          - `garnet-idle-dpm=0`     Disables activity-driven SMU GFXCLK idle governor (keeps SMUPowerState::Auto).
          - `garnet-gfxoff=0`       Disables SMU GFXOFF power-gating and its refcounted GC MMIO guard.
        """
    }
}
