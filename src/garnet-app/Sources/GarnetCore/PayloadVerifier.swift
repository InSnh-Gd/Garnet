import Foundation
import CryptoKit

public struct VerifiedPayloadInfo: Codable, Equatable, Sendable {
    public let payloadPath: String
    public let fileCount: Int
    public let defaultLayoutId: String
    public let defaultShortId: String
    public let targetSilicon: String
    public let targetPCIIDs: String
    public let releaseMode: String
    public let garnetVersion: String
    public let accelVersion: String
    public let pcibarCompatVersion: String
    public let trustAnchorEnabled: Bool
    public let trustAnchorID: String?
    public let trustAnchorSKID: String?
    public let amfiAnchorBundled: Bool
    public let recoverEFIPresent: Bool
}

public enum PayloadVerifyError: Error, CustomStringConvertible {
    case failed(String)

    public var description: String {
        switch self {
        case .failed(let msg): return msg
        }
    }
}

public enum PayloadVerifier {
    public static func sha256Hex(fileURL: URL) throws -> String {
        guard let fh = FileHandle(forReadingAtPath: fileURL.path) else {
            throw PayloadVerifyError.failed("Cannot open file for SHA-256: \(fileURL.path)")
        }
        defer { try? fh.close() }
        var hasher = SHA256()
        while true {
            let chunk = try fh.read(upToCount: 1024 * 1024) ?? Data()
            if chunk.isEmpty { break }
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    public static func parseKVFile(at url: URL) throws -> [String: String] {
        let text = try String(contentsOf: url, encoding: .utf8)
        var result: [String: String] = [:]
        for line in text.components(separatedBy: .newlines) {
            if line.isEmpty || line.hasPrefix("#") || !line.contains("=") { continue }
            let parts = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            if parts.count == 2 {
                result[String(parts[0])] = String(parts[1])
            }
        }
        return result
    }

    public static func verify(payloadURL: URL, runner: CommandRunning, verifyCodeSignatures: Bool = true) throws -> VerifiedPayloadInfo {
        let root = payloadURL.standardizedFileURL.resolvingSymlinksInPath()
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: root.path, isDirectory: &isDir), isDir.boolValue else {
            throw PayloadVerifyError.failed("Payload directory not found: \(payloadURL.path)")
        }

        // 1. Enumerate every file strictly via relative paths and forbid any symlinks anywhere inside payload.
        var actualFiles: Set<String> = []
        guard let enumr = fm.enumerator(atPath: root.path) else {
            throw PayloadVerifyError.failed("Cannot traverse payload directory")
        }
        while let rel = enumr.nextObject() as? String {
            let itemURL = root.appendingPathComponent(rel)
            var st = stat()
            guard lstat(itemURL.path, &st) == 0 else {
                throw PayloadVerifyError.failed("Cannot stat payload item: \(rel)")
            }
            let ftype = st.st_mode & S_IFMT
            if ftype == S_IFLNK {
                throw PayloadVerifyError.failed("Refusing symlink inside payload: \(itemURL.path)")
            }
            if ftype == S_IFREG {
                actualFiles.insert(rel)
            }
        }
        let resolvedPrefix = root.path.hasSuffix("/") ? root.path : root.path + "/"

        // 2. Verify SHA256SUMS covers 100% of files with zero extra/missing/mismatched files.
        let sumsURL = root.appendingPathComponent("SHA256SUMS")
        guard actualFiles.contains("SHA256SUMS") else {
            throw PayloadVerifyError.failed("Missing SHA256SUMS in payload")
        }
        let sumsText = try String(contentsOf: sumsURL, encoding: .utf8)
        var verifiedCount = 0
        var coveredFiles: Set<String> = []

        for rawLine in sumsText.components(separatedBy: .newlines) {
            if rawLine.isEmpty { continue }
            guard let sep = rawLine.range(of: "  ./") else {
                throw PayloadVerifyError.failed("Malformed SHA256SUMS line: \(rawLine)")
            }
            let expectedHex = String(rawLine[..<sep.lowerBound])
            let relPath = String(rawLine[sep.upperBound...])
            guard expectedHex.count == 64,
                  !relPath.isEmpty,
                  !relPath.hasPrefix("/"),
                  !relPath.contains("\\"),
                  !relPath.split(separator: "/").contains(".."),
                  relPath != "SHA256SUMS",
                  !coveredFiles.contains(relPath) else {
                throw PayloadVerifyError.failed("Invalid or duplicate SHA256SUMS entry: \(rawLine)")
            }
            let targetURL = root.appendingPathComponent(relPath).standardizedFileURL
            guard targetURL.path.hasPrefix(resolvedPrefix), actualFiles.contains(relPath) else {
                throw PayloadVerifyError.failed("SHA256SUMS references missing file: \(relPath)")
            }
            let actualHex = try sha256Hex(fileURL: targetURL)
            guard actualHex == expectedHex else {
                throw PayloadVerifyError.failed("SHA-256 mismatch for \(relPath): expected \(expectedHex), got \(actualHex)")
            }
            coveredFiles.insert(relPath)
            verifiedCount += 1
        }

        let expectedSet = actualFiles.subtracting(["SHA256SUMS"])
        if coveredFiles != expectedSet {
            let uncovered = expectedSet.subtracting(coveredFiles).sorted().prefix(5).joined(separator: ", ")
            throw PayloadVerifyError.failed("Payload files not covered by SHA256SUMS: \(uncovered)")
        }

        // 3. Verify MANIFEST.txt release contract.
        let manifestURL = root.appendingPathComponent("MANIFEST.txt")
        guard coveredFiles.contains("MANIFEST.txt") else {
            throw PayloadVerifyError.failed("Missing MANIFEST.txt in payload")
        }
        let manifest = try parseKVFile(at: manifestURL)
        guard manifest["FORMAT"] == "garnet-delivery-v1",
              manifest["RELEASE_MODE"] == "1",
              manifest["TARGET_SILICON"] == "navi44",
              manifest["TARGET_PCI_IDS"] == "1002:7590",
              manifest["CHIPS"] == "navi44",
              manifest["AMD_FIRMWARE_LICENSE"] == "LICENSE.amdgpu",
              manifest["GARNET_BUNDLE_ID"] == "org.garnet.driver",
              manifest["ACCEL_BUNDLE_ID"] == "org.garnet.accel",
              manifest["PCIBARCOMPAT_BUNDLE_ID"] == "org.garnet.pcibarcompat",
              let lid = manifest["DEFAULT_LAYOUT_ID"], lid.count == 64 else {
            throw PayloadVerifyError.failed("MANIFEST.txt failed Navi 44 release contract validation")
        }

        // 4. Verify required files & variants index.
        let requiredRel = [
            "LICENSE.amdgpu",
            "default-app-denylist.txt",
            "Garnet.kext/Contents/Info.plist",
            "optional/GarnetPCIBarCompat.kext/Contents/Info.plist",
            "optional/vulkan/radeon_icd.x86_64.json",
            "optional/vulkan/radv-app.sh",
            "GarnetMetal.bundle.tar",
            "variants/index.json",
            "layout-fingerprint.json",
            "activate-native.sh",
            "recover-uninstall.sh",
            "deploy-common.sh",
            "garnettest",
            "native-translate",
            "native-bundle-check",
            "publish-metal",
            "mtlprobe-direct",
            "com.garnet.native.plist",
            "com.garnet.native-translate.plist",
            "com.garnet.recovery.plist"
        ]
        for rel in requiredRel where !coveredFiles.contains(rel) {
            throw PayloadVerifyError.failed("Required release payload item missing: \(rel)")
        }
        let licenseData = try Data(contentsOf: root.appendingPathComponent("LICENSE.amdgpu"))
        if licenseData.isEmpty {
            throw PayloadVerifyError.failed("LICENSE.amdgpu must not be empty")
        }

        let recoverEFIRel = "optional/GarnetRecover.efi"
        let recoverEFIPresent = coveredFiles.contains(recoverEFIRel)
        if recoverEFIPresent {
            try verifyPE32PlusEFI(at: root.appendingPathComponent(recoverEFIRel))
        }

        let trustAnchorFlag = manifest["TRUST_ANCHOR_ENABLED"] ?? "0"
        let trustAnchorEnabled: Bool
        var trustAnchorID: String?
        var trustAnchorSKID: String?
        var amfiAnchorBundled = false
        let injectorRel = "optional/GarnetAnchorInjector.kext/Contents/Info.plist"
        if trustAnchorFlag == "1" {
            trustAnchorEnabled = true
            guard coveredFiles.contains(injectorRel) else {
                throw PayloadVerifyError.failed("TRUST_ANCHOR_ENABLED=1 requires \(injectorRel)")
            }
            let expectedAnchorID = manifest["TRUST_ANCHOR_ID"] ?? ""
            let expectedSKID = manifest["TRUST_ANCHOR_SKID"] ?? ""
            guard expectedAnchorID == "org.garnet.code-signing", expectedSKID.count == 40 else {
                throw PayloadVerifyError.failed("MANIFEST.txt has invalid TRUST_ANCHOR_ID or TRUST_ANCHOR_SKID")
            }
            try verifyAnchorInjectorPlist(
                at: root.appendingPathComponent(injectorRel),
                expectedAnchorID: expectedAnchorID,
                expectedSKIDHex: expectedSKID
            )
            trustAnchorID = expectedAnchorID
            trustAnchorSKID = expectedSKID
            if manifest["AMFIANCHOR_BUNDLED"] == "1" {
                let amfiPlistRel = "optional/AMFIAnchor.kext/Contents/Info.plist"
                guard coveredFiles.contains(amfiPlistRel) else {
                    throw PayloadVerifyError.failed("AMFIANCHOR_BUNDLED=1 requires \(amfiPlistRel)")
                }
                amfiAnchorBundled = true
            }
        } else if trustAnchorFlag == "0" {
            trustAnchorEnabled = false
            if coveredFiles.contains(injectorRel) {
                throw PayloadVerifyError.failed("Unexpected \(injectorRel) when TRUST_ANCHOR_ENABLED=0")
            }
        } else {
            throw PayloadVerifyError.failed("Invalid TRUST_ANCHOR_ENABLED value: '\(trustAnchorFlag)'")
        }

        let idxData = try Data(contentsOf: root.appendingPathComponent("variants/index.json"))
        let index = try JSONDecoder().decode(VariantsIndex.self, from: idxData)
        guard index.schema == "garnet-accel-variants-v1", index.defaultVariant == lid else {
            throw PayloadVerifyError.failed("variants/index.json schema or defaultVariant mismatch")
        }

        // 5. Strict code signature check when requested (always enforced in real macOS install/preflight).
        if verifyCodeSignatures {
            var bundlesToCheck = [
                root.appendingPathComponent("Garnet.kext").path,
                root.appendingPathComponent("optional/GarnetPCIBarCompat.kext").path
            ]
            for variant in index.variants {
                bundlesToCheck.append(root.appendingPathComponent(variant.accelBundleRelPath).path)
            }
            for bPath in bundlesToCheck {
                let res = runner.run("/usr/bin/codesign", ["--verify", "--deep", "--strict", bPath])
                if res.exitCode != 0 {
                    throw PayloadVerifyError.failed("Code signature verification failed for \(bPath): \(res.stderr)")
                }
            }
        }

        return VerifiedPayloadInfo(
            payloadPath: root.path,
            fileCount: verifiedCount,
            defaultLayoutId: lid,
            defaultShortId: String(lid.prefix(16)),
            targetSilicon: "navi44",
            targetPCIIDs: "1002:7590",
            releaseMode: "1",
            garnetVersion: manifest["GARNET_VERSION"] ?? "?",
            accelVersion: manifest["ACCEL_VERSION"] ?? "?",
            pcibarCompatVersion: manifest["PCIBARCOMPAT_VERSION"] ?? "?",
            trustAnchorEnabled: trustAnchorEnabled,
            trustAnchorID: trustAnchorID,
            trustAnchorSKID: trustAnchorSKID,
            amfiAnchorBundled: amfiAnchorBundled,
            recoverEFIPresent: recoverEFIPresent
        )
    }

    public static func verifyPE32PlusEFI(at url: URL) throws {
        let data = try Data(contentsOf: url)
        guard data.count >= 0x100, data[0] == 0x4D, data[1] == 0x5A else {
            throw PayloadVerifyError.failed("Invalid EFI binary (missing MZ header): \(url.path)")
        }
        let peOff = Int(readUInt32LE(data, offset: 0x3C))
        guard peOff >= 0, peOff + 0x70 <= data.count,
              data[peOff] == 0x50, data[peOff + 1] == 0x45, data[peOff + 2] == 0, data[peOff + 3] == 0 else {
            throw PayloadVerifyError.failed("Invalid EFI binary (missing PE header): \(url.path)")
        }
        let machine = readUInt16LE(data, offset: peOff + 4)
        let optOff = peOff + 24
        let magic = readUInt16LE(data, offset: optOff)
        let subsystem = readUInt16LE(data, offset: optOff + 68)
        guard machine == 0x8664, magic == 0x020B, subsystem == 10 else {
            throw PayloadVerifyError.failed("EFI binary is not x86_64 PE32+ EFI application: \(url.path)")
        }
    }

    private static func verifyAnchorInjectorPlist(at url: URL, expectedAnchorID: String, expectedSKIDHex: String) throws {
        let data = try Data(contentsOf: url)
        if let text = String(data: data, encoding: .utf8), text.uppercased().contains("PRIVATE KEY") {
            throw PayloadVerifyError.failed("Private key material forbidden in \(url.path)")
        }
        let obj = try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
        guard let info = obj as? [String: Any],
              (info["CFBundleIdentifier"] as? String) == "org.garnet.anchor-injector",
              info["CFBundleExecutable"] == nil,
              (info["OSBundleRequired"] as? String) == "Root",
              let libs = info["OSBundleLibraries"] as? [String: Any],
              (libs["org.b00t0x.AMFIAnchor"] as? String) == "0.1.0",
              libs["as.vit9696.Lilu"] == nil,
              let pers = info["IOKitPersonalities"] as? [String: Any],
              let p = pers["org.garnet.anchor-injector"] as? [String: Any],
              (p["CFBundleIdentifier"] as? String) == "org.b00t0x.AMFIAnchor",
              (p["IOClass"] as? String) == "AMFIAnchorProvider",
              let cfg = p["AMFIAnchorConfig"] as? [String: Any],
              (cfg["SchemaVersion"] as? Int) == 1,
              let anchors = cfg["Anchors"] as? [String: Any],
              let entry = anchors[expectedAnchorID] as? [String: Any],
              let spki = entry["SPKI"] as? Data, (64...1024).contains(spki.count),
              let skid = entry["SKID"] as? Data, skid.count == 20, skid.contains(where: { $0 != 0 }) else {
            throw PayloadVerifyError.failed("Invalid Schema-1 GarnetAnchorInjector.kext Info.plist")
        }
        let actualHex = skid.map { String(format: "%02x", $0) }.joined()
        guard actualHex == expectedSKIDHex.lowercased() else {
            throw PayloadVerifyError.failed("GarnetAnchorInjector SKID mismatch: expected \(expectedSKIDHex), got \(actualHex)")
        }
    }

    private static func readUInt16LE(_ data: Data, offset: Int) -> UInt16 {
        UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
    }

    private static func readUInt32LE(_ data: Data, offset: Int) -> UInt32 {
        UInt32(data[offset])
            | (UInt32(data[offset + 1]) << 8)
            | (UInt32(data[offset + 2]) << 16)
            | (UInt32(data[offset + 3]) << 24)
    }
}
