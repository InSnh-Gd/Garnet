import Foundation

public struct InstallOptions: Sendable {
    public let installVulkanICD: Bool
    public let dryRun: Bool
    public let verifyCodeSignatures: Bool
    public let allowNonRootSandbox: Bool

    public init(
        installVulkanICD: Bool = false,
        dryRun: Bool = false,
        verifyCodeSignatures: Bool = true,
        allowNonRootSandbox: Bool = false
    ) {
        self.installVulkanICD = installVulkanICD
        self.dryRun = dryRun
        self.verifyCodeSignatures = verifyCodeSignatures
        self.allowNonRootSandbox = allowNonRootSandbox
    }
}

public struct UninstallOptions: Sendable {
    public let removeDenylist: Bool
    public let dryRun: Bool
    public let allowNonRootSandbox: Bool

    public init(
        removeDenylist: Bool = false,
        dryRun: Bool = false,
        allowNonRootSandbox: Bool = false
    ) {
        self.removeDenylist = removeDenylist
        self.dryRun = dryRun
        self.allowNonRootSandbox = allowNonRootSandbox
    }
}

public struct AuxKCForeignKext: Codable, Equatable, Sendable {
    public let bundleID: String
    public let bundlePath: String
}

public struct InstallPlanResult: Codable, Equatable, Sendable {
    public let executed: Bool
    public let dryRun: Bool
    public let payloadVersion: String
    public let matchedLayoutId: String
    public let matchedShortId: String
    public let matchedVariantRelPath: String
    public let preservedForeignAuxKexts: [AuxKCForeignKext]
    public let plannedPaths: [String]
    public let vulkanICDInstalled: Bool
    public let denylistPreservedOnUpgrade: Bool
    public let backupDirectory: String?
    public let message: String
}

public struct UninstallPlanResult: Codable, Equatable, Sendable {
    public let executed: Bool
    public let dryRun: Bool
    public let removedPaths: [String]
    public let preservedDenylist: Bool
    public let preservedForeignAuxKexts: [AuxKCForeignKext]
    public let auxKCRemoved: Bool
    public let backupDirectory: String?
    public let message: String
}

public enum SystemInstallError: Error, CustomStringConvertible {
    case preflightFailed(String)
    case payloadInvalid(String)
    case permissionDenied(String)
    case symlinkRefused(String)
    case kmutilFailed(String)
    case transactionFailed(String)

    public var description: String {
        switch self {
        case .preflightFailed(let m): return "Preflight blocked install: \(m)"
        case .payloadInvalid(let m): return "Payload verification failed: \(m)"
        case .permissionDenied(let m): return "Administrator privileges required: \(m)"
        case .symlinkRefused(let m): return "Refusing symbolic-link target path: \(m)"
        case .kmutilFailed(let m): return "AuxKC rebuild/verification failed: \(m)"
        case .transactionFailed(let m): return "System disk transaction failed (rolled back): \(m)"
        }
    }
}

public enum SystemDiskInstaller {
    public static let garnetBundleID = "org.garnet.driver"
    public static let accelBundleID = "org.garnet.accel"

    // Matches AUX_OURS in tools/rdna4/deploy-common.sh
    public static let ourAuxKCBundleIDs: Set<String> = [
        "org.garnet.accel",
        "org.garnet.driver",
        "com.rdna4.accelprobe",
        "com.hackintosh.RDNA4FB",
        "com.hackintosh.AMDRadeonRDNA4",
        "com.rdna4.bringup",
        "com.navi48.accelprobe",
        "com.navi48.bringup"
    ]

    public static let auxKCPath = GarnetPaths.auxKCPath
    public static let extensionsDir = GarnetPaths.extensionsDir
    public static let gpuBundlesDir = GarnetPaths.gpuBundlesDir
    public static let launchDaemonsDir = GarnetPaths.launchDaemonsDir
    public static let nativeSupportDir = GarnetPaths.nativeSupportDir
    public static let vulkanICDDir = GarnetPaths.vulkanICDDir
    public static let vulkanICDPath = GarnetPaths.vulkanICDPath
    public static let radvAppBinPath = GarnetPaths.radvAppBinPath

    private static let legacyDaemonLabels: [String] = [
        "com.garnet.native",
        "com.garnet.native-translate",
        "com.rdna4.native",
        "com.rdna4.native-translate",
        "com.navi48.native",
        "com.navi48.native-translate"
    ]

    private static let retiredSystemPaths: [String] = [
        "/Library/Extensions/RDNA4Accel.kext",
        "/Library/Extensions/RDNA4Bringup.kext",
        "/Library/Extensions/RDNA4FB.kext",
        "/Library/Extensions/AMDRadeonRDNA4.kext",
        "/Library/Extensions/Navi48Accel.kext",
        "/Library/Extensions/Navi48Bringup.kext",
        "/Library/Extensions/Navi48FB.kext",
        "/Library/GPUBundles/RDNA4Metal.bundle",
        "/Library/GPUBundles/Navi48Metal.bundle",
        "/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle",
        "/Library/LaunchDaemons/com.rdna4.native.plist",
        "/Library/LaunchDaemons/com.rdna4.native-translate.plist",
        "/Library/LaunchDaemons/com.navi48.native.plist",
        "/Library/LaunchDaemons/com.navi48.native-translate.plist"
    ]

    private static let nativeSupportFiles: [String] = [
        "activate-native.sh",
        "deploy-common.sh",
        "garnettest",
        "native-translate",
        "native-bundle-check",
        "publish-metal",
        "mtlprobe-direct",
        "MANIFEST.txt",
        "layout-fingerprint.json",
        "LICENSE.amdgpu",
        "default-app-denylist.txt"
    ]

    public static func inspectAuxKCForeignKexts(env: HostEnvironment, allowUnreadableInDryRun: Bool = false) throws -> [AuxKCForeignKext] {
        let auxURL = env.url(forSystemPath: auxKCPath)
        let fm = FileManager.default
        guard fm.fileExists(atPath: auxURL.path) else {
            return []
        }
        try assertNotSymlink(url: auxURL)
        if allowUnreadableInDryRun && env.effectiveUID != 0 && env.sysRoot.path == "/" && !fm.isReadableFile(atPath: auxURL.path) {
            return []
        }

        let inspectRes = env.runner.run("/usr/bin/kmutil", [
            "inspect", "-A", auxURL.path, "--show-prelink-info", "--no-header"
        ])
        if inspectRes.exitCode != 0 {
            if allowUnreadableInDryRun && env.effectiveUID != 0 {
                return []
            }
            throw SystemInstallError.kmutilFailed("kmutil inspect \(auxURL.path) exited \(inspectRes.exitCode): \(inspectRes.stderr)")
        }

        let entries = try parsePrelinkInfoEntries(from: inspectRes.stdout)
        var result: [AuxKCForeignKext] = []
        var seenIDs: Set<String> = []
        for (bundleID, bundlePath) in entries {
            if ourAuxKCBundleIDs.contains(bundleID) { continue }
            if !seenIDs.insert(bundleID).inserted { continue }
            let resolvedURL = resolveForeignKextURL(bundlePath, env: env)
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: resolvedURL.path, isDirectory: &isDir), isDir.boolValue else {
                throw SystemInstallError.kmutilFailed("Foreign AuxKC kext missing or not a directory: \(bundleID) at \(bundlePath)")
            }
            try assertNotSymlink(url: resolvedURL)
            result.append(AuxKCForeignKext(bundleID: bundleID, bundlePath: bundlePath))
        }
        return result
    }

    public static func parsePrelinkInfoEntries(from rawOutput: String) throws -> [(bundleID: String, bundlePath: String)] {
        guard let xmlStart = rawOutput.range(of: "<?xml") ?? rawOutput.range(of: "<plist") else {
            throw SystemInstallError.kmutilFailed("kmutil --show-prelink-info output did not contain XML plist")
        }
        let xmlSlice: Substring
        if let xmlEnd = rawOutput.range(of: "</plist>", options: .backwards, range: xmlStart.lowerBound..<rawOutput.endIndex) {
            xmlSlice = rawOutput[xmlStart.lowerBound..<xmlEnd.upperBound]
        } else {
            xmlSlice = rawOutput[xmlStart.lowerBound...]
        }
        let xmlText = String(xmlSlice).replacingOccurrences(of: "\0", with: "")
        guard let data = xmlText.data(using: .utf8) else {
            throw SystemInstallError.kmutilFailed("Cannot decode prelink info UTF-8")
        }
        let obj = try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
        guard let root = obj as? [String: Any],
              let arr = root["_PrelinkInfoDictionary"] as? [[String: Any]] else {
            throw SystemInstallError.kmutilFailed("Missing _PrelinkInfoDictionary array in kmutil inspect output")
        }
        var items: [(String, String)] = []
        for item in arr {
            guard let bid = item["CFBundleIdentifier"] as? String, !bid.isEmpty,
                  let bpath = item["_PrelinkBundlePath"] as? String, !bpath.isEmpty else {
                continue
            }
            items.append((bid, bpath))
        }
        return items
    }

    public static func install(payloadURL: URL, options: InstallOptions, env: HostEnvironment) throws -> InstallPlanResult {
        let isSandbox = env.sysRoot.path != "/"
        let useSigVerify = options.verifyCodeSignatures && (!isSandbox || options.verifyCodeSignatures)

        let verified: VerifiedPayloadInfo
        do {
            verified = try PayloadVerifier.verify(
                payloadURL: payloadURL,
                runner: env.runner,
                verifyCodeSignatures: useSigVerify
            )
        } catch {
            throw SystemInstallError.payloadInvalid("\(error)")
        }

        let preflight = PreflightChecker.run(env: env, payloadURL: payloadURL)
        if !preflight.canInstall {
            // Even in sandbox mode, block hardware/OS/layout mismatches; only allow ready status
            let reasons = preflight.checks.filter { !$0.passed }.map { "\($0.name): \($0.detail)" }.joined(separator: "; ")
            throw SystemInstallError.preflightFailed(reasons.isEmpty ? "Preflight did not return ready" : reasons)
        }
        guard let layoutMatch = preflight.layoutMatch,
              layoutMatch.matched,
              let matchedLayoutId = layoutMatch.matchedLayoutId,
              let matchedShortId = layoutMatch.matchedShortId,
              let matchedVariantRel = layoutMatch.matchedVariantRelPath else {
            throw SystemInstallError.preflightFailed("No matching GarnetAccel layout variant for host KernelCollection")
        }

        if !options.dryRun {
            if !isSandbox && env.effectiveUID != 0 {
                throw SystemInstallError.permissionDenied("Real system disk install requires root privileges (use Garnet.app elevation or sudo garnetctl)")
            }
            if isSandbox && env.effectiveUID != 0 && !options.allowNonRootSandbox {
                throw SystemInstallError.permissionDenied("Sandbox install requires allowNonRootSandbox=true when non-root")
            }
        }

        let foreignKexts = try inspectAuxKCForeignKexts(env: env, allowUnreadableInDryRun: options.dryRun)
        let denylistURL = env.url(forSystemPath: DenylistManager.denylistPath)
        let legacyDenylistURL = env.url(forSystemPath: GarnetPaths.legacyDenylistPath)
        let denylistExists = FileManager.default.fileExists(atPath: denylistURL.path)
        let legacyDenylistExists = FileManager.default.fileExists(atPath: legacyDenylistURL.path)
        let willPreserveOrMigrateDenylist = denylistExists || legacyDenylistExists

        var planned = [
            "\(extensionsDir)/Garnet.kext",
            "\(extensionsDir)/GarnetAccel.kext",
            "\(gpuBundlesDir)/GarnetMetal.bundle",
            "\(launchDaemonsDir)/com.garnet.native.plist",
            "\(launchDaemonsDir)/com.garnet.native-translate.plist",
            "\(nativeSupportDir)/MANIFEST.txt",
            "\(nativeSupportDir)/profile",
            "\(nativeSupportDir)/variants/index.json"
        ]
        for f in nativeSupportFiles where f != "MANIFEST.txt" {
            planned.append("\(nativeSupportDir)/\(f)")
        }
        if !denylistExists {
            planned.append(DenylistManager.denylistPath)
        }
        if options.installVulkanICD {
            planned.append(vulkanICDPath)
            planned.append(radvAppBinPath)
        }
        planned.append(auxKCPath)

        if options.dryRun {
            return InstallPlanResult(
                executed: false,
                dryRun: true,
                payloadVersion: verified.garnetVersion,
                matchedLayoutId: matchedLayoutId,
                matchedShortId: matchedShortId,
                matchedVariantRelPath: matchedVariantRel,
                preservedForeignAuxKexts: foreignKexts,
                plannedPaths: planned,
                vulkanICDInstalled: options.installVulkanICD,
                denylistPreservedOnUpgrade: willPreserveOrMigrateDenylist,
                backupDirectory: nil,
                message: "Dry-run verified payload \(verified.garnetVersion) and layout \(matchedShortId); ESP untouched; \(foreignKexts.count) foreign AuxKC kext(s) preserved"
            )
        }

        let fm = FileManager.default
        let payloadRoot = payloadURL.standardizedFileURL.resolvingSymlinksInPath()

        // Verify all parent directories & target paths are not symlinks
        for abs in [
            extensionsDir, gpuBundlesDir, launchDaemonsDir,
            GarnetPaths.supportRoot, nativeSupportDir, GarnetPaths.backupsDir,
            GarnetPaths.logsDir, GarnetPaths.translateLogsDir,
            GarnetPaths.xlateDumpDir, GarnetPaths.spvCacheDir,
            "/Library/KernelCollections"
        ] {
            try assertNotSymlink(url: env.url(forSystemPath: abs))
        }
        if options.installVulkanICD {
            for abs in ["/Library/Vulkan", vulkanICDDir, "/usr/local", "/usr/local/bin"] {
                try assertNotSymlink(url: env.url(forSystemPath: abs))
            }
        }
        for abs in planned + retiredSystemPaths + GarnetPaths.legacyRuntimeCleanupPaths {
            try assertNotSymlink(url: env.url(forSystemPath: abs))
        }
        if legacyDenylistExists {
            try assertNotSymlink(url: legacyDenylistURL)
        }

        // Stage extracted GarnetMetal.bundle outside target paths first and verify codesign before mutating disk
        let stageDir = fm.temporaryDirectory.appendingPathComponent("garnet-install-stage-\(UUID().uuidString)")
        try fm.createDirectory(at: stageDir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: stageDir) }

        let tarURL = payloadRoot.appendingPathComponent("GarnetMetal.bundle.tar")
        let extractDir = stageDir.appendingPathComponent("metal-extract")
        try fm.createDirectory(at: extractDir, withIntermediateDirectories: true)
        let untar = env.runner.run("/usr/bin/tar", ["-xf", tarURL.path, "-C", extractDir.path])
        if untar.exitCode != 0 {
            throw SystemInstallError.payloadInvalid("Failed extracting GarnetMetal.bundle.tar: \(untar.stderr)")
        }
        let stagedMetal = extractDir.appendingPathComponent("GarnetMetal.bundle")
        guard fm.fileExists(atPath: stagedMetal.path) else {
            throw SystemInstallError.payloadInvalid("GarnetMetal.bundle missing after extracting tarball")
        }
        if useSigVerify {
            let cs = env.runner.run("/usr/bin/codesign", ["--verify", "--deep", "--strict", stagedMetal.path])
            if cs.exitCode != 0 {
                throw SystemInstallError.payloadInvalid("GarnetMetal.bundle code signature failed: \(cs.stderr)")
            }
        }

        // Unload existing LaunchDaemons cleanly when modifying live "/"
        if !isSandbox {
            for lbl in legacyDaemonLabels {
                _ = env.runner.run("/bin/launchctl", ["bootout", "system/\(lbl)"])
            }
        }

        // Create backup directory for atomic rollback
        let backupsRoot = env.url(forSystemPath: GarnetPaths.backupsDir)
        let timestamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "")
        let backupDir = backupsRoot.appendingPathComponent("install-\(timestamp)-\(UUID().uuidString.prefix(6))")
        try fm.createDirectory(at: backupDir, withIntermediateDirectories: true)

        var journal: [(target: URL, backup: URL?)] = []

        func recordAndBackup(target: URL, idx: Int) throws {
            try assertNotSymlink(url: target)
            if fm.fileExists(atPath: target.path) {
                let bak = backupDir.appendingPathComponent("item-\(idx)-\(target.lastPathComponent)")
                try fm.copyItem(at: target, to: bak)
                journal.append((target, bak))
            } else {
                journal.append((target, nil))
            }
        }

        func rollback() {
            for item in journal.reversed() {
                try? fm.removeItem(at: item.target)
                if let bak = item.backup {
                    try? fm.createDirectory(at: item.target.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try? fm.copyItem(at: bak, to: item.target)
                }
            }
        }

        var idx = 0
        do {
            // Backup targets that will be written or cleaned
            var targetsToTrack = planned.filter { $0 != DenylistManager.denylistPath }
            if !denylistExists {
                targetsToTrack.append(DenylistManager.denylistPath)
            }
            for retired in retiredSystemPaths + GarnetPaths.legacyRuntimeCleanupPaths
                where fm.fileExists(atPath: env.url(forSystemPath: retired).path) {
                targetsToTrack.append(retired)
            }
            for abs in targetsToTrack {
                try recordAndBackup(target: env.url(forSystemPath: abs), idx: idx)
                idx += 1
            }

            // Ensure target directories exist
            for dirAbs in [
                extensionsDir, gpuBundlesDir, launchDaemonsDir,
                nativeSupportDir, "\(nativeSupportDir)/variants",
                "/Library/KernelCollections",
                GarnetPaths.logsDir, GarnetPaths.translateLogsDir,
                GarnetPaths.xlateDumpDir, GarnetPaths.spvCacheDir
            ] {
                try fm.createDirectory(at: env.url(forSystemPath: dirAbs), withIntermediateDirectories: true)
            }
            if options.installVulkanICD {
                for dirAbs in [vulkanICDDir, "/usr/local/bin"] {
                    try fm.createDirectory(at: env.url(forSystemPath: dirAbs), withIntermediateDirectories: true)
                }
            }

            // Remove retired system files if present
            for retired in retiredSystemPaths {
                let rURL = env.url(forSystemPath: retired)
                if fm.fileExists(atPath: rURL.path) {
                    try fm.removeItem(at: rURL)
                }
            }

            // Copy Garnet.kext & matched GarnetAccel.kext (NEVER copy GarnetPCIBarCompat.kext here!)
            let garnetDst = env.url(forSystemPath: "\(extensionsDir)/Garnet.kext")
            try replaceItem(from: payloadRoot.appendingPathComponent("Garnet.kext"), to: garnetDst)

            let accelSrc = payloadRoot.appendingPathComponent(matchedVariantRel)
            let accelDst = env.url(forSystemPath: "\(extensionsDir)/GarnetAccel.kext")
            try replaceItem(from: accelSrc, to: accelDst)

            // Copy GarnetMetal.bundle
            let metalDst = env.url(forSystemPath: "\(gpuBundlesDir)/GarnetMetal.bundle")
            try replaceItem(from: stagedMetal, to: metalDst)

            // Copy native support tools & metadata
            for rel in nativeSupportFiles {
                let src = payloadRoot.appendingPathComponent(rel)
                let dst = env.url(forSystemPath: "\(nativeSupportDir)/\(rel)")
                try replaceItem(from: src, to: dst)
            }
            let varIdxDst = env.url(forSystemPath: "\(nativeSupportDir)/variants/index.json")
            try replaceItem(from: payloadRoot.appendingPathComponent("variants/index.json"), to: varIdxDst)
            let profileDst = env.url(forSystemPath: "\(nativeSupportDir)/profile")
            try Data("native\n".utf8).write(to: profileDst, options: .atomic)
            try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: profileDst.path)

            // Idempotent v0.1.0 -> Garnet app-denylist migration BEFORE cleaning legacy RDNA4/native:
            // preserve existing Garnet denylist; else migrate legacy RDNA4 denylist; else seed default.
            if !denylistExists {
                if legacyDenylistExists {
                    try replaceItem(from: legacyDenylistURL, to: denylistURL)
                } else {
                    try replaceItem(from: payloadRoot.appendingPathComponent("default-app-denylist.txt"), to: denylistURL)
                }
            }

            // Clean legacy v0.1.0 runtime dirs (NEVER touches RDNA4/hardware-test or its backups)
            for legacyAbs in GarnetPaths.legacyRuntimeCleanupPaths {
                let legURL = env.url(forSystemPath: legacyAbs)
                if fm.fileExists(atPath: legURL.path) {
                    try fm.removeItem(at: legURL)
                }
            }

            // Copy LaunchDaemons
            for plistName in ["com.garnet.native.plist", "com.garnet.native-translate.plist"] {
                let src = payloadRoot.appendingPathComponent(plistName)
                let dst = env.url(forSystemPath: "\(launchDaemonsDir)/\(plistName)")
                try replaceItem(from: src, to: dst)
            }

            // Copy optional Vulkan ICD & radv-app ONLY if explicitly enabled
            if options.installVulkanICD {
                try replaceItem(
                    from: payloadRoot.appendingPathComponent("optional/vulkan/radeon_icd.x86_64.json"),
                    to: env.url(forSystemPath: vulkanICDPath)
                )
                let radvDst = env.url(forSystemPath: radvAppBinPath)
                try replaceItem(
                    from: payloadRoot.appendingPathComponent("optional/vulkan/radv-app.sh"),
                    to: radvDst
                )
                try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: radvDst.path)
            }

            // Permissions & ownership on real system root (set permissions AFTER chown so special bits like 01777 stick)
            let execNames = ["activate-native.sh", "garnettest", "native-translate", "native-bundle-check", "publish-metal", "mtlprobe-direct"]
            for name in execNames {
                let p = env.url(forSystemPath: "\(nativeSupportDir)/\(name)").path
                try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: p)
            }
            if !isSandbox && env.effectiveUID == 0 {
                var chownPaths = [garnetDst.path, accelDst.path, metalDst.path,
                                  env.url(forSystemPath: "\(launchDaemonsDir)/com.garnet.native.plist").path,
                                  env.url(forSystemPath: "\(launchDaemonsDir)/com.garnet.native-translate.plist").path,
                                  env.url(forSystemPath: GarnetPaths.supportRoot).path,
                                  env.url(forSystemPath: GarnetPaths.xlateDumpDir).path]
                if options.installVulkanICD {
                    chownPaths.append(env.url(forSystemPath: vulkanICDPath).path)
                    chownPaths.append(env.url(forSystemPath: radvAppBinPath).path)
                }
                for p in chownPaths {
                    let ch = env.runner.run("/usr/sbin/chown", ["-R", "root:wheel", p])
                    if ch.exitCode != 0 {
                        throw SystemInstallError.transactionFailed("chown root:wheel \(p) failed: \(ch.stderr)")
                    }
                }
                let cm = env.runner.run("/bin/chmod", ["-R", "go-w", garnetDst.path, accelDst.path, metalDst.path])
                if cm.exitCode != 0 {
                    throw SystemInstallError.transactionFailed("chmod go-w failed: \(cm.stderr)")
                }
                for ld in ["com.garnet.native.plist", "com.garnet.native-translate.plist"] {
                    try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: env.url(forSystemPath: "\(launchDaemonsDir)/\(ld)").path)
                }
                let chSpv = env.runner.run("/usr/sbin/chown", ["-hR", "nobody:nobody", GarnetPaths.spvCacheDir])
                if chSpv.exitCode != 0 {
                    throw SystemInstallError.transactionFailed("chown nobody:nobody \(GarnetPaths.spvCacheDir) failed: \(chSpv.stderr)")
                }
                let chLog = env.runner.run("/usr/sbin/chown", ["nobody:nobody", GarnetPaths.translateLogsDir])
                if chLog.exitCode != 0 {
                    throw SystemInstallError.transactionFailed("chown nobody:nobody \(GarnetPaths.translateLogsDir) failed: \(chLog.stderr)")
                }
            }
            try fm.setAttributes([.posixPermissions: 0o1777], ofItemAtPath: env.url(forSystemPath: GarnetPaths.xlateDumpDir).path)
            try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: env.url(forSystemPath: GarnetPaths.spvCacheDir).path)

            // Build temporary AuxKC with preserved foreign kexts + Garnet.kext + GarnetAccel.kext
            let tmpAuxURL = stageDir.appendingPathComponent("AuxiliaryKernelExtensions.kc")
            var kmArgs = [
                "create", "-n", "aux", "-x", "-z", "--force", "--allow-missing-kdk",
                "-B", env.url(forSystemPath: env.bootKCPath).path,
                "-S", env.url(forSystemPath: env.systemKCPath).path,
                "-A", tmpAuxURL.path
            ]
            for fk in foreignKexts {
                kmArgs.append("-p")
                kmArgs.append(resolveForeignKextURL(fk.bundlePath, env: env).path)
            }
            kmArgs.append("-p")
            kmArgs.append(garnetDst.path)
            kmArgs.append("-p")
            kmArgs.append(accelDst.path)

            let kmCreate = env.runner.run("/usr/bin/kmutil", kmArgs)
            if kmCreate.exitCode != 0 {
                throw SystemInstallError.kmutilFailed("kmutil create aux failed (\(kmCreate.exitCode)): \(kmCreate.stderr)")
            }

            // Verify newly built AuxKC includes our two kexts AND all preserved foreign kexts
            let kmVerify = env.runner.run("/usr/bin/kmutil", ["inspect", "-A", tmpAuxURL.path, "--show-prelink-info", "--no-header"])
            if kmVerify.exitCode != 0 {
                throw SystemInstallError.kmutilFailed("kmutil verify new AuxKC failed: \(kmVerify.stderr)")
            }
            let verifiedEntries = try parsePrelinkInfoEntries(from: kmVerify.stdout)
            let verifiedIDs = Set(verifiedEntries.map { $0.bundleID })
            guard verifiedIDs.contains(garnetBundleID), verifiedIDs.contains(accelBundleID) else {
                throw SystemInstallError.kmutilFailed("Rebuilt AuxKC missing \(garnetBundleID) or \(accelBundleID)")
            }
            for fk in foreignKexts where !verifiedIDs.contains(fk.bundleID) {
                throw SystemInstallError.kmutilFailed("Rebuilt AuxKC dropped foreign kext \(fk.bundleID)")
            }

            let auxDst = env.url(forSystemPath: auxKCPath)
            try replaceItem(from: tmpAuxURL, to: auxDst)
            try? fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: auxDst.path)
            if !isSandbox && env.effectiveUID == 0 {
                _ = env.runner.run("/usr/sbin/chown", ["root:wheel", auxDst.path])
            }
        } catch let err as SystemInstallError {
            rollback()
            throw err
        } catch {
            rollback()
            throw SystemInstallError.transactionFailed("\(error)")
        }

        let clearedIconCaches = clearIconCachesBestEffort(env: env)
        let cacheNote = clearedIconCaches.isEmpty ? "" : "; cleared \(clearedIconCaches.count) macOS icon cache path(s)"

        return InstallPlanResult(
            executed: true,
            dryRun: false,
            payloadVersion: verified.garnetVersion,
            matchedLayoutId: matchedLayoutId,
            matchedShortId: matchedShortId,
            matchedVariantRelPath: matchedVariantRel,
            preservedForeignAuxKexts: foreignKexts,
            plannedPaths: planned,
            vulkanICDInstalled: options.installVulkanICD,
            denylistPreservedOnUpgrade: willPreserveOrMigrateDenylist,
            backupDirectory: backupDir.path,
            message: "Installed Garnet \(verified.garnetVersion) (layout \(matchedShortId)) to system disk & AuxKC\(cacheNote); reboot required to activate. ESP was not modified."
        )
    }

    public static func uninstall(options: UninstallOptions, env: HostEnvironment) throws -> UninstallPlanResult {
        let isSandbox = env.sysRoot.path != "/"
        if !options.dryRun {
            if !isSandbox && env.effectiveUID != 0 {
                throw SystemInstallError.permissionDenied("Real system disk uninstall requires root privileges")
            }
            if isSandbox && env.effectiveUID != 0 && !options.allowNonRootSandbox {
                throw SystemInstallError.permissionDenied("Sandbox uninstall requires allowNonRootSandbox=true when non-root")
            }
        }

        let foreignKexts = try inspectAuxKCForeignKexts(env: env, allowUnreadableInDryRun: options.dryRun)

        var managedPaths = [
            "\(extensionsDir)/Garnet.kext",
            "\(extensionsDir)/GarnetAccel.kext",
            "\(gpuBundlesDir)/GarnetMetal.bundle",
            "\(launchDaemonsDir)/com.garnet.native.plist",
            "\(launchDaemonsDir)/com.garnet.native-translate.plist",
            vulkanICDPath,
            radvAppBinPath
        ]
        for rel in nativeSupportFiles {
            managedPaths.append("\(nativeSupportDir)/\(rel)")
        }
        managedPaths.append("\(nativeSupportDir)/profile")
        managedPaths.append("\(nativeSupportDir)/variants/index.json")
        managedPaths.append(GarnetPaths.xlateDumpDir)
        managedPaths.append(GarnetPaths.spvCacheDir)
        if options.removeDenylist {
            managedPaths.append(DenylistManager.denylistPath)
        }
        managedPaths.append(contentsOf: retiredSystemPaths)
        managedPaths.append(contentsOf: GarnetPaths.legacyRuntimeCleanupPaths)

        let fm = FileManager.default
        // Enumerate runtime/kill-switch markers under /private/tmp and /private/var/tmp matching garnet-* or rdna4m-*
        var seenPaths = Set(managedPaths)
        for tmpDir in ["/private/tmp", "/private/var/tmp"] {
            let tmpURL = env.url(forSystemPath: tmpDir)
            if let names = try? fm.contentsOfDirectory(atPath: tmpURL.path) {
                for name in names.sorted() where name.hasPrefix("garnet-") || name.hasPrefix("rdna4m-") {
                    let candidate = "\(tmpDir)/\(name)"
                    let candURL = env.url(forSystemPath: candidate)
                    if let attrs = try? fm.attributesOfItem(atPath: candURL.path),
                       (attrs[.type] as? FileAttributeType) == .typeSymbolicLink {
                        continue
                    }
                    if seenPaths.insert(candidate).inserted {
                        managedPaths.append(candidate)
                    }
                }
            }
        }

        let newDenylistURL = env.url(forSystemPath: DenylistManager.denylistPath)
        let legacyDenylistURL = env.url(forSystemPath: GarnetPaths.legacyDenylistPath)
        let migrateLegacyDenylistBeforeCleanup = !options.removeDenylist
            && !fm.fileExists(atPath: newDenylistURL.path)
            && fm.fileExists(atPath: legacyDenylistURL.path)

        let existingTargets = managedPaths.filter { fm.fileExists(atPath: env.url(forSystemPath: $0).path) }
        let auxRemoved = foreignKexts.isEmpty

        if options.dryRun {
            return UninstallPlanResult(
                executed: false,
                dryRun: true,
                removedPaths: existingTargets,
                preservedDenylist: !options.removeDenylist,
                preservedForeignAuxKexts: foreignKexts,
                auxKCRemoved: auxRemoved,
                backupDirectory: nil,
                message: "Dry-run: would remove \(existingTargets.count) path(s), preserve \(foreignKexts.count) foreign AuxKC kext(s); ESP untouched"
            )
        }

        for abs in existingTargets + [auxKCPath] {
            try assertNotSymlink(url: env.url(forSystemPath: abs))
        }
        if migrateLegacyDenylistBeforeCleanup {
            try assertNotSymlink(url: legacyDenylistURL)
            try assertNotSymlink(url: newDenylistURL)
        }

        if !isSandbox {
            for lbl in legacyDaemonLabels {
                _ = env.runner.run("/bin/launchctl", ["bootout", "system/\(lbl)"])
            }
        }

        let backupsRoot = env.url(forSystemPath: GarnetPaths.backupsDir)
        let timestamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "")
        let backupDir = backupsRoot.appendingPathComponent("uninstall-\(timestamp)-\(UUID().uuidString.prefix(6))")
        try fm.createDirectory(at: backupDir, withIntermediateDirectories: true)

        var journal: [(target: URL, backup: URL?)] = []
        func rollback() {
            for item in journal.reversed() {
                try? fm.removeItem(at: item.target)
                if let bak = item.backup {
                    try? fm.createDirectory(at: item.target.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try? fm.copyItem(at: bak, to: item.target)
                }
            }
        }

        do {
            for (idx, abs) in (existingTargets + [auxKCPath]).enumerated() {
                let u = env.url(forSystemPath: abs)
                if fm.fileExists(atPath: u.path) {
                    let bak = backupDir.appendingPathComponent("item-\(idx)-\(u.lastPathComponent)")
                    try fm.copyItem(at: u, to: bak)
                    journal.append((u, bak))
                }
            }

            // Preserve unmigrated v0.1.0 denylist before deleting legacy RDNA4/native directory
            if migrateLegacyDenylistBeforeCleanup {
                try fm.createDirectory(at: newDenylistURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fm.copyItem(at: legacyDenylistURL, to: newDenylistURL)
                try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: newDenylistURL.path)
                journal.append((newDenylistURL, nil))
            }

            for abs in existingTargets {
                let u = env.url(forSystemPath: abs)
                if fm.fileExists(atPath: u.path) {
                    try fm.removeItem(at: u)
                }
            }
            let variantsDir = env.url(forSystemPath: "\(nativeSupportDir)/variants")
            if fm.fileExists(atPath: variantsDir.path) {
                try? fm.removeItem(at: variantsDir)
            }

            let auxURL = env.url(forSystemPath: auxKCPath)
            if foreignKexts.isEmpty {
                if fm.fileExists(atPath: auxURL.path) {
                    try fm.removeItem(at: auxURL)
                }
            } else {
                let stageDir = fm.temporaryDirectory.appendingPathComponent("garnet-uninstall-stage-\(UUID().uuidString)")
                try fm.createDirectory(at: stageDir, withIntermediateDirectories: true)
                defer { try? fm.removeItem(at: stageDir) }
                let tmpAuxURL = stageDir.appendingPathComponent("AuxiliaryKernelExtensions.kc")
                var kmArgs = [
                    "create", "-n", "aux", "-x", "-z", "--force", "--allow-missing-kdk",
                    "-B", env.url(forSystemPath: env.bootKCPath).path,
                    "-S", env.url(forSystemPath: env.systemKCPath).path,
                    "-A", tmpAuxURL.path
                ]
                for fk in foreignKexts {
                    kmArgs.append("-p")
                    kmArgs.append(resolveForeignKextURL(fk.bundlePath, env: env).path)
                }
                let kmCreate = env.runner.run("/usr/bin/kmutil", kmArgs)
                if kmCreate.exitCode != 0 {
                    throw SystemInstallError.kmutilFailed("kmutil create aux during uninstall failed: \(kmCreate.stderr)")
                }
                let kmVerify = env.runner.run("/usr/bin/kmutil", ["inspect", "-A", tmpAuxURL.path, "--show-prelink-info", "--no-header"])
                if kmVerify.exitCode != 0 {
                    throw SystemInstallError.kmutilFailed("kmutil inspect during uninstall failed: \(kmVerify.stderr)")
                }
                let verifiedIDs = Set(try parsePrelinkInfoEntries(from: kmVerify.stdout).map { $0.bundleID })
                for fk in foreignKexts where !verifiedIDs.contains(fk.bundleID) {
                    throw SystemInstallError.kmutilFailed("Uninstall AuxKC rebuild dropped foreign kext \(fk.bundleID)")
                }
                try replaceItem(from: tmpAuxURL, to: auxURL)
            }
        } catch let err as SystemInstallError {
            rollback()
            throw err
        } catch {
            rollback()
            throw SystemInstallError.transactionFailed("\(error)")
        }

        let clearedIconCaches = clearIconCachesBestEffort(env: env)
        let cacheNote = clearedIconCaches.isEmpty ? "" : "; cleared \(clearedIconCaches.count) macOS icon cache path(s)"

        return UninstallPlanResult(
            executed: true,
            dryRun: false,
            removedPaths: existingTargets,
            preservedDenylist: !options.removeDenylist,
            preservedForeignAuxKexts: foreignKexts,
            auxKCRemoved: auxRemoved,
            backupDirectory: backupDir.path,
            message: "Uninstalled Garnet system-disk components (\(foreignKexts.count) foreign AuxKC kext(s) preserved)\(cacheNote); reboot required. Remember to disable GarnetPCIBarCompat.kext / garnet* boot-args in OpenCore if reverting completely."
        )
    }

    public static func exportPCIBarCompat(fromPayload payloadURL: URL, toDestination destDir: URL) throws -> URL {
        let src = payloadURL.appendingPathComponent("optional/GarnetPCIBarCompat.kext")
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: src.path, isDirectory: &isDir), isDir.boolValue else {
            throw SystemInstallError.payloadInvalid("optional/GarnetPCIBarCompat.kext missing in payload")
        }
        try FileManager.default.createDirectory(at: destDir, withIntermediateDirectories: true)
        let dst = destDir.appendingPathComponent("GarnetPCIBarCompat.kext")
        try replaceItem(from: src, to: dst)
        return dst
    }

    /// Best-effort deletion of macOS icon bitmap caches so iconservicesagent and Dock regenerate
    /// fresh icons after rebooting into a new (or removed) GarnetMetal stack. Never kills running
    /// daemons, never follows symlinks, and ignores missing/EPERM paths so install/uninstall cannot fail.
    @discardableResult
    public static func clearIconCachesBestEffort(env: HostEnvironment) -> [String] {
        let fm = FileManager.default
        var cleared: [String] = []

        func isRegularOrDir(_ url: URL) -> Bool {
            var st = stat()
            guard lstat(url.path, &st) == 0 else { return false }
            let kind = st.st_mode & S_IFMT
            return kind == S_IFREG || kind == S_IFDIR
        }

        func isRealDir(_ url: URL) -> Bool {
            var st = stat()
            return lstat(url.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
        }

        let sysStore = env.url(forSystemPath: GarnetPaths.systemIconCacheStorePath)
        if isRegularOrDir(sysStore) {
            do {
                try fm.removeItem(at: sysStore)
                cleared.append(GarnetPaths.systemIconCacheStorePath)
            } catch {}
        }

        let varFoldersURL = env.url(forSystemPath: GarnetPaths.varFoldersDir)
        if isRealDir(varFoldersURL),
           let buckets = try? fm.contentsOfDirectory(atPath: varFoldersURL.path) {
            for bucket in buckets.sorted() {
                let bucketURL = varFoldersURL.appendingPathComponent(bucket)
                guard isRealDir(bucketURL),
                      let users = try? fm.contentsOfDirectory(atPath: bucketURL.path) else {
                    continue
                }
                for userHash in users.sorted() {
                    let cDirURL = bucketURL.appendingPathComponent(userHash).appendingPathComponent("C")
                    guard isRealDir(cDirURL) else { continue }
                    for targetName in ["com.apple.iconservices", "com.apple.dock.iconcache"] {
                        let targetURL = cDirURL.appendingPathComponent(targetName)
                        guard isRegularOrDir(targetURL) else { continue }
                        do {
                            try fm.removeItem(at: targetURL)
                            cleared.append("\(GarnetPaths.varFoldersDir)/\(bucket)/\(userHash)/C/\(targetName)")
                        } catch {}
                    }
                }
            }
        }
        return cleared
    }

    private static func resolveForeignKextURL(_ bundlePath: String, env: HostEnvironment) -> URL {
        if env.sysRoot.path != "/" {
            let prefix = env.sysRoot.path.hasSuffix("/") ? env.sysRoot.path : env.sysRoot.path + "/"
            if bundlePath.hasPrefix(prefix) {
                return URL(fileURLWithPath: bundlePath)
            }
        }
        return env.url(forSystemPath: bundlePath)
    }

    private static func assertNotSymlink(url: URL) throws {
        var st = stat()
        if lstat(url.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK {
            throw SystemInstallError.symlinkRefused(url.path)
        }
    }

    private static func replaceItem(from src: URL, to dst: URL) throws {
        let fm = FileManager.default
        try assertNotSymlink(url: dst)
        if fm.fileExists(atPath: dst.path) {
            try fm.removeItem(at: dst)
        }
        try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.copyItem(at: src, to: dst)
    }
}
