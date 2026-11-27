import Foundation

public enum PreflightStatus: String, Codable, Equatable, Sendable {
    case ready
    case sipSetupRequired = "sip-setup-required"
    case blocked
}

public enum BARRecommendation: String, Codable, Equatable, Sendable {
    case fullBar = "full-bar"
    case small256M = "256m-small-bar"
}

public struct PreflightCheckItem: Codable, Equatable, Sendable {
    public let name: String
    public let passed: Bool
    public let level: String   // "PASS", "WARN", "ERROR"
    public let detail: String
}

public struct PreflightReport: Codable, Equatable, Sendable {
    public let overallStatus: PreflightStatus
    public let canInstall: Bool
    public let macOSProductVersion: String
    public let macOSBuildVersion: String
    public let arch: String
    public let isVM: Bool
    public let detectedGPUs: [PCIGPUInfo]
    public let sip: SIPStatusInfo
    public let layoutMatch: LayoutMatchResult?
    public let pciBarCompat: PCIBarCompatResult?
    public let barRecommendation: BARRecommendation
    public let barReason: String
    public let biosReminder: String
    public let checks: [PreflightCheckItem]
}

public enum PreflightChecker {
    public static func run(env: HostEnvironment, payloadURL: URL) -> PreflightReport {
        var checks: [PreflightCheckItem] = []
        var hardFailed = false

        let ver = env.readProductVersion()
        let build = env.readBuildVersion()
        let osOk = HostEnvironment.isMacOS26Version(ver)
        if !osOk { hardFailed = true }
        checks.append(PreflightCheckItem(
            name: "macOS Version",
            passed: osOk,
            level: osOk ? "PASS" : "ERROR",
            detail: osOk ? "macOS \(ver) (\(build))" : "Requires macOS 26.x, found '\(ver)' (\(build))"
        ))

        let arch = env.readArch()
        let archOk = (arch == "x86_64")
        if !archOk { hardFailed = true }
        checks.append(PreflightCheckItem(
            name: "Architecture",
            passed: archOk,
            level: archOk ? "PASS" : "ERROR",
            detail: archOk ? "x86_64" : "Requires x86_64, found '\(arch)'"
        ))

        let isVM = env.readIsVM()
        let vmOk = !isVM
        if !vmOk { hardFailed = true }
        checks.append(PreflightCheckItem(
            name: "Bare Metal",
            passed: vmOk,
            level: vmOk ? "PASS" : "ERROR",
            detail: vmOk ? "Physical x86_64 host (no VMM hypervisor bit)" : "Virtual machine detected (VMM flag set); bare-metal RX 9060 XT required"
        ))

        let gpus = env.readPCIGPUs()
        let hasNavi44 = gpus.contains(where: { $0.isNavi44 })
        let hasNavi48 = gpus.contains(where: { $0.isNavi48 })
        let gpuOk = hasNavi44 && !hasNavi48
        if !gpuOk { hardFailed = true }
        let gpuDetail: String
        if gpuOk {
            let names = gpus.map { "\($0.model) [\($0.pciIDString)]" }.joined(separator: ", ")
            gpuDetail = "Verified Navi 44 (1002:7590) present (\(names))"
        } else if hasNavi48 {
            gpuDetail = "Unsupported/unvalidated Navi 48 GPU detected (\(gpus.map { $0.pciIDString }.joined(separator: ", "))); this release supports Navi 44 (1002:7590) only"
        } else {
            gpuDetail = "No AMD Radeon RX 9060 XT (1002:7590) found (detected: \(gpus.isEmpty ? "none" : gpus.map { $0.pciIDString }.joined(separator: ", ")))"
        }
        checks.append(PreflightCheckItem(
            name: "GPU Hardware",
            passed: gpuOk,
            level: gpuOk ? "PASS" : "ERROR",
            detail: gpuDetail
        ))

        // KC layout verification (always runs even if SIP isn't open yet so user knows before rebooting)
        var layoutResult: LayoutMatchResult?
        let sysKCOk: Bool
        do {
            let res = try MachOKCVerifier.matchVariant(
                variantsDirPayload: payloadURL,
                bootKCPath: env.url(forSystemPath: env.bootKCPath).path,
                systemKCPath: env.url(forSystemPath: env.systemKCPath).path
            )
            layoutResult = res
            sysKCOk = res.matched
            if !res.matched { hardFailed = true }
            checks.append(PreflightCheckItem(
                name: "KernelCollection Layout",
                passed: res.matched,
                level: res.matched ? "PASS" : "ERROR",
                detail: res.summary
            ))
        } catch {
            sysKCOk = false
            hardFailed = true
            checks.append(PreflightCheckItem(
                name: "KernelCollection Layout",
                passed: false,
                level: "ERROR",
                detail: "KC layout inspection failed: \(error)"
            ))
        }

        // IOPCIFamily compatibility + PFM64 check -> Full-BAR (8/16 GiB) vs 256 MiB Small-BAR recommendation
        let bootKCFile = env.url(forSystemPath: env.bootKCPath).path
        let compat = PCIBarCompatCheck.checkBootKC(path: bootKCFile)
        checks.append(PreflightCheckItem(
            name: "IOPCIFamily Full-BAR Compatibility",
            passed: true,
            level: compat.compatible ? "PASS" : "WARN",
            detail: compat.compatible
                ? "Compatible via \(compat.mode.rawValue) (UUID \(compat.iopciFamilyUUID ?? "?"), entry \(compat.entryHdrOffsetHex ?? "?"))"
                : "Not compatible for Full-BAR hook (\(compat.status), UUID \(compat.iopciFamilyUUID ?? "?")); recommend 256 MiB Small-BAR fallback"
        ))

        // Check if current running session has GarnetDriver/PFM64 hints or small-BAR fallback indicator
        let drvIoreg = env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetDriver", "-d", "2"]).stdout
        let compatIoreg = env.runner.run("/usr/sbin/ioreg", ["-rc", "GarnetPCIBarCompat", "-d", "1"]).stdout
        let pfm64Insufficient = compatIoreg.contains("PFM64Insufficient") || drvIoreg.contains("PFM64Insufficient")

        let barRec: BARRecommendation
        let barReason: String
        if compat.compatible && !pfm64Insufficient {
            barRec = .fullBar
            barReason = "Recommended default: Full-BAR (8/16 GiB; IOPCIFamily verified via \(compat.mode.rawValue)); confirm BIOS Above 4G Decoding & Re-Size BAR are Enabled."
        } else {
            barRec = .small256M
            if pfm64Insufficient {
                barReason = "Fallback to 256 MiB Small-BAR recommended: host bridge 64-bit PFM64 window insufficient for Full-BAR aperture."
            } else {
                barReason = "Fallback to 256 MiB Small-BAR recommended: IOPCIFamily (\(compat.status)) did not match structural/exact-uuid gates."
            }
        }

        // SIP gate (can be resolved by user via OpenCore reboot)
        let sip = env.readSIPStatus()
        checks.append(PreflightCheckItem(
            name: "SIP / AuxKC Policy",
            passed: sip.readyForAuxKC,
            level: sip.readyForAuxKC ? "PASS" : "WARN",
            detail: sip.detail
        ))

        let overall: PreflightStatus
        if hardFailed || !sysKCOk {
            overall = .blocked
        } else if !sip.readyForAuxKC {
            overall = .sipSetupRequired
        } else {
            overall = .ready
        }

        return PreflightReport(
            overallStatus: overall,
            canInstall: overall == .ready,
            macOSProductVersion: ver,
            macOSBuildVersion: build,
            arch: arch,
            isVM: isVM,
            detectedGPUs: gpus,
            sip: sip,
            layoutMatch: layoutResult,
            pciBarCompat: compat,
            barRecommendation: barRec,
            barReason: barReason,
            biosReminder: "Enable 'Above 4G Decoding' and 'Re-Size BAR Support' in motherboard BIOS, and disable CSM.",
            checks: checks
        )
    }
}
