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
    public let kextInstalledPath: String?
    public let mode: OpenCoreMode
    public let report: OpenCoreReport
}

public enum OpenCoreValidator {
    public static let nvramBootGUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"

    private static let forbiddenSystemKexts: [(pattern: String, why: String)] = [
        ("Garnet.kext", "Must live in /Library/Extensions (AuxKC), never in OpenCore EFI/OC/Kexts/ (requires SystemKC IOGraphicsFamily)"),
        ("GarnetAccel.kext", "Must live in /Library/Extensions (AuxKC), never in OpenCore EFI/OC/Kexts/"),
        ("AMDRadeonRDNA4.kext", "Retired legacy Lilu plugin; must be disabled/removed"),
        ("RDNA4Bringup.kext", "Retired legacy bringup kext; must be disabled/removed"),
        ("RDNA4FB.kext", "Retired legacy framebuffer kext; must be disabled/removed"),
        ("RDNA4Accel.kext", "Retired legacy accel kext; must be disabled/removed"),
        ("Navi48Bringup.kext", "Retired Navi 48 bringup kext; must be disabled/removed")
    ]

    private static let requiredBootArgs: [String] = [
        "garnet=1",
        "garnet-interrupts=1",
        "garnet-stage=17",
        "garnet-native=1",
        "garnet-metal=1",
        "garnet-metal-disp=1",
        "garnet-metal-ws=1",
        "garnet-multisession=1",
        "garnet-apps=1",
        "amfi=0x80"
    ]

    public static func validate(configURL: URL, mode: OpenCoreMode) throws -> OpenCoreReport {
        // Strictly read-only: open read bytes without ever writing or re-serializing the source file.
        let rawData = try Data(contentsOf: configURL, options: .mappedIfSafe)
        let plistAny = try PropertyListSerialization.propertyList(from: rawData, options: [], format: nil)
        guard let root = plistAny as? [String: Any] else {
            return OpenCoreReport(
                configPath: configURL.path,
                mode: mode,
                valid: false,
                errorCount: 1,
                warnCount: 0,
                findings: [OpenCoreFinding(severity: .error, keyPath: "/", message: "Root object of config.plist must be a dictionary")],
                readOnlyVerified: true
            )
        }

        var findings: [OpenCoreFinding] = []

        // 1. Kernel -> Add checks (placement contrast + GarnetPCIBarCompat mode rules)
        let kernelAdd = ((root["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]]) ?? []
        var pciCompatEntry: [String: Any]?

        for entry in kernelAdd {
            let bundlePath = (entry["BundlePath"] as? String) ?? ""
            let enabled = (entry["Enabled"] as? Bool) ?? false
            let baseName = (bundlePath as NSString).lastPathComponent

            if baseName == "GarnetPCIBarCompat.kext" {
                pciCompatEntry = entry
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

        // 2. NVRAM -> Add -> 7C436110... (boot-args + csr-active-config) & Delete
        let nvramAdd = (((root["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[nvramBootGUID] as? [String: Any]) ?? [:]
        let bootArgsStr = (nvramAdd["boot-args"] as? String) ?? ""
        let argTokens = bootArgsStr.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        let argSet = Set(argTokens)

        let missingArgs = requiredBootArgs.filter { !argSet.contains($0) }
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

        // 3. Misc -> Security -> SecureBootModel
        let secModel = (((root["Misc"] as? [String: Any])?["Security"] as? [String: Any])?["SecureBootModel"] as? String) ?? ""
        if secModel == "Disabled" {
            findings.append(OpenCoreFinding(severity: .pass, keyPath: "Misc.Security.SecureBootModel", message: "SecureBootModel = Disabled"))
        } else {
            findings.append(OpenCoreFinding(severity: .error, keyPath: "Misc.Security.SecureBootModel", message: "SecureBootModel must be 'Disabled' (got '\(secModel)')"))
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

    /// Back up config.plist, export GarnetPCIBarCompat.kext (Full-BAR mode), apply Garnet OpenCore settings, and verify.
    public static func configure(targetURL: URL, mode: OpenCoreMode, payloadURL: URL?) throws -> OpenCoreConfigureResult {
        let configURL = try resolveConfigURL(from: targetURL)
        let rawData = try Data(contentsOf: configURL)
        let plistAny = try PropertyListSerialization.propertyList(from: rawData, options: [], format: nil)
        guard var root = plistAny as? [String: Any] else {
            throw OpenCoreConfigureError.failed("Root object of \(configURL.path) must be a dictionary")
        }

        if mode == .full && payloadURL == nil {
            throw OpenCoreConfigureError.failed("Full-BAR (8/16 GiB) mode requires a valid payload to copy GarnetPCIBarCompat.kext")
        }

        let backupURL = try writeTimestampedBackup(rawData: rawData, nextTo: configURL)

        var kextInstalledPath: String?
        if mode == .full, let payloadURL {
            let kextsDir = configURL.deletingLastPathComponent().appendingPathComponent("Kexts")
            try rejectSymlink(at: kextsDir)
            let dst = try SystemDiskInstaller.exportPCIBarCompat(fromPayload: payloadURL, toDestination: kextsDir)
            kextInstalledPath = dst.path
        }

        applyKernelSettings(to: &root, mode: mode)
        applyQuirks(to: &root, mode: mode)
        applySecurityAndNVRAM(to: &root, mode: mode)

        let newData = try PropertyListSerialization.data(fromPropertyList: root, format: .xml, options: 0)
        try newData.write(to: configURL, options: .atomic)

        let report = try validate(configURL: configURL, mode: mode)
        guard report.valid else {
            try? rawData.write(to: configURL, options: .atomic)
            let issues = report.findings.filter { $0.severity == .error }.map { "\($0.keyPath): \($0.message)" }.joined(separator: "; ")
            throw OpenCoreConfigureError.failed("Post-write validation failed (restored original config.plist): \(issues)")
        }

        return OpenCoreConfigureResult(
            configPath: configURL.path,
            backupPath: backupURL.path,
            kextInstalledPath: kextInstalledPath,
            mode: mode,
            report: report
        )
    }

    private static func rejectSymlink(at url: URL) throws {
        var st = stat()
        if lstat(url.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK {
            throw OpenCoreConfigureError.failed("Refusing symbolic link path: \(url.path)")
        }
    }

    private static func writeTimestampedBackup(rawData: Data, nextTo configURL: URL) throws -> URL {
        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.dateFormat = "yyyyMMdd-HHmmss"
        let baseName = "\(configURL.lastPathComponent).garnet-backup-\(df.string(from: Date()))"
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

    private static func applyKernelSettings(to root: inout [String: Any], mode: OpenCoreMode) {
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

        var quirks = (kernel["Quirks"] as? [String: Any]) ?? [:]
        quirks["IncreasePciBarSize"] = false
        kernel["Add"] = kernelAdd
        kernel["Quirks"] = quirks
        root["Kernel"] = kernel
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

    private static func applySecurityAndNVRAM(to root: inout [String: Any], mode: OpenCoreMode) {
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
        bootDict["boot-args"] = updatedBootArgs(from: existingArgs, mode: mode)
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

    private static func updatedBootArgs(from existing: String, mode: OpenCoreMode) -> String {
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

        kept.append(contentsOf: requiredBootArgs)
        kept.append("garnet-apps-all=1")
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

    public static func sampleConfigSnippet(mode: OpenCoreMode) -> String {
        switch mode {
        case .full:
            return """
            # Full-BAR (8/16 GiB) OpenCore Checklist
            1. Copy `GarnetPCIBarCompat.kext` to `EFI/OC/Kexts/` and add & Enable in `Kernel -> Add`
               (Place AFTER Lilu/WEG/VirtualSMC; set MaxKernel="" MinKernel="25.0.0").
            2. NEVER put Garnet.kext or GarnetAccel.kext inside `EFI/OC/Kexts/`.
            3. `Booter -> Quirks -> ResizeAppleGpuBars` = -1
               `UEFI -> Quirks -> ResizeGpuBars` = -1
               `Kernel -> Quirks -> IncreasePciBarSize` = False
            4. `Misc -> Security -> SecureBootModel` = "Disabled"
            5. `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`:
               csr-active-config = <03080000>  (0x0803)
               boot-args = -v keepsyms=1 garnet=1 garnet-interrupts=1 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80 garnet-full-bar=1
               (DO NOT include npci=0x2000 or -radvesa)
            6. `NVRAM -> Delete -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`: add `boot-args` and `csr-active-config`.
            """
        case .modeSmall:
            return """
            # 256 MiB Small-BAR OpenCore Checklist (Fallback / Recovery)
            1. Remove or Disable `GarnetPCIBarCompat.kext` in `Kernel -> Add`.
            2. `Booter -> Quirks -> ResizeAppleGpuBars` = 0
               `UEFI -> Quirks -> ResizeGpuBars` = 0
               `Kernel -> Quirks -> IncreasePciBarSize` = False
            3. `Misc -> Security -> SecureBootModel` = "Disabled"
            4. `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`:
               csr-active-config = <03080000>  (0x0803)
               boot-args = -v keepsyms=1 npci=0x2000 garnet=1 garnet-interrupts=1 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80
               (DO NOT include garnet-full-bar=1 or -radvesa)
            5. `NVRAM -> Delete -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`: add `boot-args` and `csr-active-config`.
            """
        }
    }

    public static func emergencyKillSwitchesText() -> String {
        """
        Emergency OpenCore boot-arg kill-switches (add to boot-args at OpenCore Picker / config.plist):
          - `garnet=0`              Disables Garnet.kext primary driver & GPU init completely (boots on VESA/fallback display).
          - `garnet-aux=0`          Disables GarnetAccel.kext / GarnetFramebuffer while keeping bringup diagnostics accessible.
          - `garnet-rebar-compat=0` Disables GarnetPCIBarCompat.kext Full-BAR hook without removing it from EFI/OC/Kexts/.
          - `garnet-apps-all=0`     Disables global desktop app Metal access while preserving core compositor & allowlisted tools.
          - `garnet-irq-wait=0`     Reverts kernel fence waits to legacy polling without disabling MSI interrupts.
        """
    }
}
