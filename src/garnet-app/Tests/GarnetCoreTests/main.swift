import Foundation
import CryptoKit
#if canImport(GarnetCore)
import GarnetCore
#endif

@main
struct GarnetCoreTestsRunner {
    private static var passed = 0
    private static var failed = 0

    static func check(_ name: String, _ cond: @autoclosure () -> Bool, detail: String = "") {
        if cond() {
            passed += 1
        } else {
            failed += 1
            fputs("FAIL: \(name)\(detail.isEmpty ? "" : " (\(detail))")\n", stderr)
        }
    }

    static func sha256Hex(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    // MARK: - Little-Endian Binary Builders for Synthetic Mach-O / KC Tests

    private static func appendLE32(_ d: inout Data, _ v: UInt32) {
        var le = v.littleEndian
        withUnsafeBytes(of: &le) { d.append(contentsOf: $0) }
    }

    private static func appendLE64(_ d: inout Data, _ v: UInt64) {
        var le = v.littleEndian
        withUnsafeBytes(of: &le) { d.append(contentsOf: $0) }
    }

    private static func appendFixed16(_ d: inout Data, _ s: String) {
        var bytes = Array(s.utf8.prefix(16))
        while bytes.count < 16 { bytes.append(0) }
        d.append(contentsOf: bytes)
    }

    private static func makeIOPCIMachO(uuid: [UInt8], textSize: Int = 0x50000, textBodyPatch: (inout [UInt8], Int) -> Void) -> Data {
        // Build a valid MH_KEXT_BUNDLE with LC_UUID + LC_SEGMENT_64(__TEXT -> __text)
        let hdrSize = 32
        let lcUuidSize = 24
        let lcSegSize = 72 + 80
        let cmdsSize = lcUuidSize + lcSegSize
        let textRel = 0x1000
        let totalSize = textRel + textSize

        var data = Data(count: totalSize)
        func writeLE32(at off: Int, _ v: UInt32) {
            var le = v.littleEndian
            withUnsafeBytes(of: &le) { data.replaceSubrange(off..<(off + 4), with: $0) }
        }
        func writeLE64(at off: Int, _ v: UInt64) {
            var le = v.littleEndian
            withUnsafeBytes(of: &le) { data.replaceSubrange(off..<(off + 8), with: $0) }
        }
        func writeFixed16(at off: Int, _ s: String) {
            var b = Array(s.utf8.prefix(16))
            while b.count < 16 { b.append(0) }
            data.replaceSubrange(off..<(off + 16), with: b)
        }

        // mach_header_64
        writeLE32(at: 0, 0xFEEDFACF)
        writeLE32(at: 4, 0x01000007)
        writeLE32(at: 8, 3)
        writeLE32(at: 12, 11) // MH_KEXT_BUNDLE
        writeLE32(at: 16, 2)  // ncmds
        writeLE32(at: 20, UInt32(cmdsSize))

        // LC_UUID
        writeLE32(at: hdrSize, 0x1B)
        writeLE32(at: hdrSize + 4, UInt32(lcUuidSize))
        data.replaceSubrange((hdrSize + 8)..<(hdrSize + 24), with: uuid)

        // LC_SEGMENT_64 __TEXT + __text
        let segOff = hdrSize + lcUuidSize
        let vmBase: UInt64 = 0x100000
        writeLE32(at: segOff, 0x19)
        writeLE32(at: segOff + 4, UInt32(lcSegSize))
        writeFixed16(at: segOff + 8, "__TEXT")
        writeLE64(at: segOff + 24, vmBase)
        writeLE64(at: segOff + 32, UInt64(totalSize))
        writeLE64(at: segOff + 40, 0)
        writeLE64(at: segOff + 48, UInt64(totalSize))
        writeLE32(at: segOff + 64, 1)

        let sectOff = segOff + 72
        writeFixed16(at: sectOff, "__text")
        writeFixed16(at: sectOff + 16, "__TEXT")
        writeLE64(at: sectOff + 32, vmBase + UInt64(textRel))
        writeLE64(at: sectOff + 40, UInt64(textSize))
        writeLE32(at: sectOff + 48, UInt32(textRel))

        var body = [UInt8](repeating: 0x90, count: textSize)
        textBodyPatch(&body, textRel)
        data.replaceSubrange(textRel..<(textRel + textSize), with: body)
        return data
    }

    private static func populateValidIOPCIFunc(bytes: inout [UInt8], textRel: Int, entryHdrOff: Int) {
        let e = entryHdrOff - textRel
        let prologue: [UInt8] = [
            0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
            0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
            0x58, 0x89, 0x4D, 0xB4, 0x41, 0x89, 0xD6, 0x49,
            0x89, 0xF5, 0x48, 0x89
        ]
        let site1: [UInt8] = [0x48, 0x3D, 0x01, 0x00, 0x00, 0x40, 0x44, 0x8B, 0x75, 0xD4, 0x41, 0x83, 0xD6, 0xFF]
        let site2: [UInt8] = [0x48, 0x3D, 0x01, 0x00, 0x00, 0x40, 0x89, 0xC1, 0x48, 0x0F, 0x42, 0xC8, 0x0F, 0x92, 0xC0, 0x20, 0xC2]
        let ctxR16: [UInt8] = [0xBA, 0x04, 0x00, 0x00, 0x00, 0x31, 0xC9]
        let ctxW16: [UInt8] = [0xBA, 0x04, 0x00, 0x00, 0x00, 0x45, 0x31, 0xC0]
        let ctxR32: [UInt8] = [0x4C, 0x89, 0xEE, 0x44, 0x89, 0xFA, 0x31, 0xC9]
        let ctxW32: [UInt8] = [0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0x45, 0x31, 0xC0]
        let anchorClass: [UInt8] = [0x41, 0x0F, 0xB7, 0x45, 0x2E, 0xC1, 0xE0, 0x10, 0x3D, 0x00, 0x00, 0x03, 0x00]
        let anchorBus: [UInt8] = [0x48, 0x8B, 0x45, 0xC8, 0x80, 0x78, 0x32, 0x00]
        let anchorInit: [UInt8] = [0x48, 0x8B, 0x55, 0xC8, 0x4C, 0x8B, 0x74, 0xC2, 0x60, 0x4C, 0x89, 0xF7, 0x44, 0x89, 0xEE, 0x4C, 0x89, 0xFA, 0x49, 0x89, 0xC8]
        let anchorFin: [UInt8] = [0x49, 0x89, 0x46, 0x40, 0x41, 0x80, 0xFC, 0x01, 0x75, 0x1A, 0x41, 0x80, 0x4E, 0x54, 0x80, 0x48, 0x8B, 0x45, 0xC0, 0xF6, 0x80, 0x91, 0x00, 0x00, 0x00, 0x20, 0x74, 0x08, 0x49, 0xC7, 0x46, 0x48, 0xFF, 0xFF, 0xFF, 0xFF]

        func put(_ off: Int, _ pat: [UInt8]) {
            for i in 0..<pat.count { bytes[e + off + i] = pat[i] }
        }
        put(0, prologue)
        put(0x1D, ctxR16)
        put(0x3E, ctxW16)
        put(0xBF, anchorClass)
        put(0x11C, ctxR32)
        put(0x134, ctxW32)
        put(0x197, anchorBus)
        put(0x2B2, site1)
        put(0x3B7, site2)
        put(0x3CE, anchorInit)
        put(0x3F6, anchorFin)

        let helpers: [(Int, Int)] = [
            (0x24, 0x306D8 - textRel),
            (0x46, 0x32AE6 - textRel),
            (0x124, 0x2F9E0 - textRel),
            (0x13C, 0x32B6A - textRel),
            (0x3E3, 0x3EEBF - textRel)
        ]
        for (site, helperTextOff) in helpers {
            bytes[helperTextOff] = 0x55
            bytes[helperTextOff + 1] = 0x48
            bytes[helperTextOff + 2] = 0x89
            bytes[helperTextOff + 3] = 0xE5
            let rel = Int32(helperTextOff - (e + site + 5))
            bytes[e + site] = 0xE8
            var le = rel.littleEndian
            withUnsafeBytes(of: &le) { raw in
                for k in 0..<4 { bytes[e + site + 1 + k] = raw[k] }
            }
        }
    }

    private struct SyntheticKCFixture {
        let dir: URL
        let bootKC: URL
        let sysKC: URL
        let payloadDir: URL
        let layoutId: String
        let shortId: String
    }

    private static func buildSyntheticKCsAndPayload(root: URL, iopciData: Data) throws -> SyntheticKCFixture {
        let fm = FileManager.default
        let kcDir = root.appendingPathComponent("kcs")
        try fm.createDirectory(at: kcDir, withIntermediateDirectories: true)

        // Build BootKC as MH_FILESET containing com.apple.iokit.IOPCIFamily + ___cxa_pure_virtual sym
        let bootBase = StreamingKCImage.bootKernelChainedBase
        let iopciFileOff: UInt64 = 0x4000
        let bootSymFileOff: UInt64 = UInt64(0x4000 + iopciData.count)
        let pureVirtualVA: UInt64 = bootBase + 0x2000

        var bootStr = Data([0])
        let pureStrOff = UInt32(bootStr.count)
        bootStr.append(contentsOf: "___cxa_pure_virtual\0".utf8)
        let bootSymSize = 16
        let bootStrFileOff = bootSymFileOff + UInt64(bootSymSize)
        let bootTotalSize = Int(bootStrFileOff) + bootStr.count

        // Top header: 2 load cmds: LC_SEGMENT_64 (72) + LC_FILESET_ENTRY (32+32=64) + LC_SYMTAB (24) = 160
        let fsCmdSize = 32 + 32
        let bootCmdsSize = 72 + fsCmdSize + 24
        var bootKCData = Data(count: bootTotalSize)
        func bootLE32(_ off: Int, _ v: UInt32) {
            var le = v.littleEndian; withUnsafeBytes(of: &le) { bootKCData.replaceSubrange(off..<(off+4), with: $0) }
        }
        func bootLE64(_ off: Int, _ v: UInt64) {
            var le = v.littleEndian; withUnsafeBytes(of: &le) { bootKCData.replaceSubrange(off..<(off+8), with: $0) }
        }
        func bootStr16(_ off: Int, _ s: String) {
            var b = Array(s.utf8.prefix(16)); while b.count < 16 { b.append(0) }
            bootKCData.replaceSubrange(off..<(off+16), with: b)
        }
        bootLE32(0, 0xFEEDFACF); bootLE32(4, 0x01000007); bootLE32(8, 3); bootLE32(12, 12)
        bootLE32(16, 3); bootLE32(20, UInt32(bootCmdsSize))
        // LC_SEGMENT_64 __TEXT mapping entire bootKC at bootBase
        bootLE32(32, 0x19); bootLE32(36, 72); bootStr16(40, "__TEXT")
        bootLE64(56, bootBase); bootLE64(64, UInt64(bootTotalSize))
        bootLE64(72, 0); bootLE64(80, UInt64(bootTotalSize))
        // LC_FILESET_ENTRY for com.apple.iokit.IOPCIFamily
        let bfsOff = 32 + 72
        bootLE32(bfsOff, 0x80000035); bootLE32(bfsOff + 4, UInt32(fsCmdSize))
        bootLE64(bfsOff + 8, bootBase + iopciFileOff); bootLE64(bfsOff + 16, iopciFileOff)
        bootLE32(bfsOff + 24, 32)
        bootKCData.replaceSubrange((bfsOff + 32)..<(bfsOff + 32 + "com.apple.iokit.IOPCIFamily\0".utf8.count), with: "com.apple.iokit.IOPCIFamily\0".utf8)
        // LC_SYMTAB
        let bsOff = bfsOff + fsCmdSize
        bootLE32(bsOff, 0x2); bootLE32(bsOff + 4, 24)
        bootLE32(bsOff + 8, UInt32(bootSymFileOff)); bootLE32(bsOff + 12, 1)
        bootLE32(bsOff + 16, UInt32(bootStrFileOff)); bootLE32(bsOff + 20, UInt32(bootStr.count))

        // Patch iopciData's __TEXT.__text fileoff relative to bootKC before embedding
        var adjustedIopci = iopciData
        let textFileOffInSlice = 0x1000
        var adjustedOffLE = UInt32(Int(iopciFileOff) + textFileOffInSlice).littleEndian
        withUnsafeBytes(of: &adjustedOffLE) { adjustedIopci.replaceSubrange((32 + 24 + 72 + 48)..<(32 + 24 + 72 + 52), with: $0) }
        bootKCData.replaceSubrange(Int(iopciFileOff)..<(Int(iopciFileOff) + adjustedIopci.count), with: adjustedIopci)

        bootLE32(Int(bootSymFileOff), pureStrOff)
        bootKCData[Int(bootSymFileOff) + 4] = 0x0F
        bootLE64(Int(bootSymFileOff) + 8, pureVirtualVA)
        bootKCData.replaceSubrange(Int(bootStrFileOff)..<(Int(bootStrFileOff) + bootStr.count), with: bootStr)

        let bootKCURL = kcDir.appendingPathComponent("BootKernelExtensions.kc")
        try bootKCData.write(to: bootKCURL)

        // Build SystemKC as MH_FILESET containing 13 classes with total 2720 slots (12*200 + 320 = 2720)
        // SystemKC single segment __TEXT at vmBase = 0x100000
        let sysVMBase: UInt64 = 0x100000
        let dataStart = 0x4000
        var bodyData = Data()
        var symEntries: [(name: String, va: UInt64)] = []
        var fpClasses: [FPClassEntry] = []

        func currentVA() -> UInt64 { sysVMBase + UInt64(dataStart + bodyData.count) }
        let inheritedSymName = "__ZN11SharedBase3fooEv"
        let inheritedSymVA = currentVA()
        bodyData.append(contentsOf: [0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90])
        symEntries.append((inheritedSymName, inheritedSymVA))

        for i in 0..<13 {
            let parent = "GarnetBase\(i)"
            let leaf = "GarnetLeaf\(i)"
            let nslots = (i < 12) ? 200 : 320
            let instSize = 128 + i * 16

            // Class name cstring
            let cstrVA = currentVA()
            bodyData.append(contentsOf: "\(parent)\0".utf8)
            while bodyData.count % 8 != 0 { bodyData.append(0) }

            // MetaClass::MetaClass() stub matching MachOKCParser.extractClassInstanceSize
            // 48 8D 35 <rel32 to cstrVA>; 31 D2; B9 <uint32 instSize>; C3
            let ctorVA = currentVA()
            let leaRel = Int32(Int64(cstrVA) - Int64(ctorVA + 7))
            bodyData.append(contentsOf: [0x48, 0x8D, 0x35])
            appendLE32(&bodyData, UInt32(bitPattern: leaRel))
            bodyData.append(contentsOf: [0x31, 0xD2, 0xB9])
            appendLE32(&bodyData, UInt32(instSize))
            bodyData.append(0xC3)
            while bodyData.count % 8 != 0 { bodyData.append(0) }
            symEntries.append(("__ZN\(MachOKCVerifier.mangleClass(parent))9MetaClassC2Ev", ctorVA))

            // Vtable: 16B header + nslots*8 + 8B sentinel + 8B next symbol marker
            let vtVA = currentVA()
            appendLE64(&bodyData, 0); appendLE64(&bodyData, 0)
            var overrides: [Int] = []
            for slot in 0..<nslots {
                if slot == 2 {
                    // Inherited slot pointing to systemKC sym via lvl=1 chained fixup
                    let fixup = (UInt64(1) << 30) | (inheritedSymVA & 0x3FFFFFFF)
                    appendLE64(&bodyData, fixup)
                } else {
                    overrides.append(slot)
                    // Non-pure slot (any non-zero value that isn't pureVA)
                    let ownFixup = (UInt64(1) << 30) | (ctorVA & 0x3FFFFFFF)
                    appendLE64(&bodyData, ownFixup)
                }
            }
            let afterVtVA = currentVA()
            appendLE64(&bodyData, 0)
            symEntries.append(("__ZTV\(MachOKCVerifier.mangleClass(parent))", vtVA))
            symEntries.append(("__Z_after_\(i)", afterVtVA))

            fpClasses.append(FPClassEntry(
                leaf: leaf,
                parent: parent,
                instanceSize: instSize,
                nslots: nslots,
                overrideSlots: overrides,
                inheritedBindings: [FPInheritedBinding(slot: 2, syms: [inheritedSymName])]
            ))
        }

        // Build LC_SYMTAB tables for SystemKC
        var sysStrTab = Data([0])
        var sysSymTab = Data()
        for (nm, va) in symEntries {
            let off = UInt32(sysStrTab.count)
            sysStrTab.append(contentsOf: "\(nm)\0".utf8)
            appendLE32(&sysSymTab, off)
            sysSymTab.append(0x0F)
            sysSymTab.append(1)
            sysSymTab.append(contentsOf: [0, 0])
            appendLE64(&sysSymTab, va)
        }
        let sysSymOff = dataStart + bodyData.count
        let sysStrOff = sysSymOff + sysSymTab.count
        let sysTotalSize = sysStrOff + sysStrTab.count
        let sysCmdsSize = 72 + 24

        var sysKCData = Data(count: sysTotalSize)
        func sysLE32(_ off: Int, _ v: UInt32) {
            var le = v.littleEndian; withUnsafeBytes(of: &le) { sysKCData.replaceSubrange(off..<(off+4), with: $0) }
        }
        func sysLE64(_ off: Int, _ v: UInt64) {
            var le = v.littleEndian; withUnsafeBytes(of: &le) { sysKCData.replaceSubrange(off..<(off+8), with: $0) }
        }
        func sysStr16(_ off: Int, _ s: String) {
            var b = Array(s.utf8.prefix(16)); while b.count < 16 { b.append(0) }
            sysKCData.replaceSubrange(off..<(off+16), with: b)
        }
        sysLE32(0, 0xFEEDFACF); sysLE32(4, 0x01000007); sysLE32(8, 3); sysLE32(12, 12)
        sysLE32(16, 2); sysLE32(20, UInt32(sysCmdsSize))
        sysLE32(32, 0x19); sysLE32(36, 72); sysStr16(40, "__TEXT")
        sysLE64(56, sysVMBase); sysLE64(64, UInt64(sysTotalSize))
        sysLE64(72, 0); sysLE64(80, UInt64(sysTotalSize))
        sysLE32(104, 0x2); sysLE32(108, 24)
        sysLE32(112, UInt32(sysSymOff)); sysLE32(116, UInt32(symEntries.count))
        sysLE32(120, UInt32(sysStrOff)); sysLE32(124, UInt32(sysStrTab.count))

        sysKCData.replaceSubrange(dataStart..<(dataStart + bodyData.count), with: bodyData)
        sysKCData.replaceSubrange(sysSymOff..<(sysSymOff + sysSymTab.count), with: sysSymTab)
        sysKCData.replaceSubrange(sysStrOff..<(sysStrOff + sysStrTab.count), with: sysStrTab)
        let sysKCURL = kcDir.appendingPathComponent("SystemKernelExtensions.kc")
        try sysKCData.write(to: sysKCURL)

        // Compute layout fingerprint and assemble full synthetic Release 1 payload
        let tempFP = LayoutFingerprint(
            schema: "garnet-layout-fingerprint-v1",
            arch: "x86_64",
            expectClasses: 13,
            expectSlots: 2720,
            layoutId: "",
            shortId: "",
            classes: fpClasses
        )
        let lid = try MachOKCVerifier.canonicalLayoutID(for: tempFP)
        let sid = String(lid.prefix(16))
        let fp = LayoutFingerprint(
            schema: "garnet-layout-fingerprint-v1",
            arch: "x86_64",
            expectClasses: 13,
            expectSlots: 2720,
            layoutId: lid,
            shortId: sid,
            classes: fpClasses
        )

        let payloadDir = root.appendingPathComponent("payload")
        try makeCompleteReleasePayload(at: payloadDir, fingerprint: fp)

        return SyntheticKCFixture(
            dir: kcDir, bootKC: bootKCURL, sysKC: sysKCURL,
            payloadDir: payloadDir, layoutId: lid, shortId: sid
        )
    }

    private static func makeCompleteReleasePayload(at payload: URL, fingerprint: LayoutFingerprint) throws {
        let fm = FileManager.default
        try? fm.removeItem(at: payload)
        try fm.createDirectory(at: payload, withIntermediateDirectories: true)

        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        let fpData = try enc.encode(fingerprint)
        try fpData.write(to: payload.appendingPathComponent("layout-fingerprint.json"))

        let variantRel = "variants/\(fingerprint.shortId)/GarnetAccel.kext"
        let variantFP = "variants/\(fingerprint.shortId)/layout-fingerprint.json"
        let vDir = payload.appendingPathComponent("variants/\(fingerprint.shortId)")
        try fm.createDirectory(at: vDir, withIntermediateDirectories: true)
        try fpData.write(to: payload.appendingPathComponent(variantFP))

        let idx = VariantsIndex(
            schema: "garnet-accel-variants-v1",
            defaultVariant: fingerprint.layoutId,
            variants: [
                VariantIndexEntry(
                    layoutId: fingerprint.layoutId,
                    shortId: fingerprint.shortId,
                    accelBundleRelPath: variantRel,
                    fingerprintRelPath: variantFP,
                    verifiedBuilds: ["25G332"]
                )
            ]
        )
        try enc.encode(idx).write(to: payload.appendingPathComponent("variants/index.json"))

        func writeDummyBundle(at dir: URL, bid: String, exec: String, ver: String) throws {
            try fm.createDirectory(at: dir.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
            let plist = """
            <?xml version="1.0" encoding="UTF-8"?>
            <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
            <plist version="1.0"><dict>
            <key>CFBundleIdentifier</key><string>\(bid)</string>
            <key>CFBundleExecutable</key><string>\(exec)</string>
            <key>CFBundleVersion</key><string>\(ver)</string>
            </dict></plist>
            """
            try plist.write(to: dir.appendingPathComponent("Contents/Info.plist"), atomically: true, encoding: .utf8)
            try Data("dummy-macho-\(exec)".utf8).write(to: dir.appendingPathComponent("Contents/MacOS/\(exec)"))
        }

        try writeDummyBundle(at: payload.appendingPathComponent("Garnet.kext"), bid: "org.garnet.driver", exec: "Garnet", ver: "0.0.673")
        try writeDummyBundle(at: vDir.appendingPathComponent("GarnetAccel.kext"), bid: "org.garnet.accel", exec: "GarnetAccel", ver: "0.0.8")
        try writeDummyBundle(at: payload.appendingPathComponent("optional/GarnetPCIBarCompat.kext"), bid: "org.garnet.pcibarcompat", exec: "GarnetPCIBarCompat", ver: "1.0.0")

        // Create real GarnetMetal.bundle.tar using system tar so SystemDiskInstaller extraction works
        let tmpBundleRoot = payload.appendingPathComponent(".metal-stage")
        try fm.createDirectory(at: tmpBundleRoot.appendingPathComponent("GarnetMetal.bundle/Contents/MacOS"), withIntermediateDirectories: true)
        try fm.createDirectory(at: tmpBundleRoot.appendingPathComponent("GarnetMetal.bundle/Contents/Resources"), withIntermediateDirectories: true)
        try Data("metal-info".utf8).write(to: tmpBundleRoot.appendingPathComponent("GarnetMetal.bundle/Contents/Info.plist"))
        try Data("metal-bin".utf8).write(to: tmpBundleRoot.appendingPathComponent("GarnetMetal.bundle/Contents/MacOS/GarnetMetal"))
        try Data("radv-bin".utf8).write(to: tmpBundleRoot.appendingPathComponent("GarnetMetal.bundle/Contents/Resources/libvulkan_radeon.dylib"))
        try Data("xlate-bin".utf8).write(to: tmpBundleRoot.appendingPathComponent("GarnetMetal.bundle/Contents/Resources/libgarnetxlate.dylib"))
        let tarRes = ProcessCommandRunner().run("/usr/bin/tar", [
            "-cf", payload.appendingPathComponent("GarnetMetal.bundle.tar").path,
            "-C", tmpBundleRoot.path, "GarnetMetal.bundle"
        ])
        guard tarRes.exitCode == 0 else {
            throw NSError(domain: "GarnetTests", code: 1, userInfo: [NSLocalizedDescriptionKey: "tar build failed: \(tarRes.stderr)"])
        }
        try fm.removeItem(at: tmpBundleRoot)

        try fm.createDirectory(at: payload.appendingPathComponent("optional/vulkan"), withIntermediateDirectories: true)
        try Data("{\"file_format_version\":\"1.0.0\"}\n".utf8).write(to: payload.appendingPathComponent("optional/vulkan/radeon_icd.x86_64.json"))
        try Data("#!/bin/bash\nexit 0\n".utf8).write(to: payload.appendingPathComponent("optional/vulkan/radv-app.sh"))
        try Data("AMD firmware redistribution license fixture\n".utf8).write(to: payload.appendingPathComponent("LICENSE.amdgpu"))
        try Data("# default denylist\nproc:DefaultBlockedApp\n".utf8).write(to: payload.appendingPathComponent("default-app-denylist.txt"))

        for rel in [
            "activate-native.sh", "deploy-common.sh", "garnettest", "native-translate",
            "native-bundle-check", "publish-metal", "mtlprobe-direct",
            "com.garnet.native.plist", "com.garnet.native-translate.plist"
        ] {
            try Data("fixture-\(rel)\n".utf8).write(to: payload.appendingPathComponent(rel))
        }

        let manifestText = """
        FORMAT=garnet-delivery-v1
        RELEASE_MODE=1
        TARGET_SILICON=navi44
        TARGET_PCI_IDS=1002:7590
        CHIPS=navi44
        DEFAULT_LAYOUT_ID=\(fingerprint.layoutId)
        AMD_FIRMWARE_LICENSE=LICENSE.amdgpu
        GARNET_BUNDLE_ID=org.garnet.driver
        GARNET_VERSION=0.0.673
        ACCEL_BUNDLE_ID=org.garnet.accel
        ACCEL_VERSION=0.0.8
        PCIBARCOMPAT_BUNDLE_ID=org.garnet.pcibarcompat
        PCIBARCOMPAT_VERSION=1.0.0
        #
        # Payload files

        SHA256 content files (SHA256SUMS also covers MANIFEST.txt):
        """
        try manifestText.write(to: payload.appendingPathComponent("MANIFEST.txt"), atomically: true, encoding: .utf8)
        try regenerateSHA256SUMS(at: payload)
    }

    private static func regenerateSHA256SUMS(at payload: URL) throws {
        let fm = FileManager.default
        let resolvedRoot = payload.standardizedFileURL.resolvingSymlinksInPath()
        var relPaths: [String] = []
        if let e = fm.enumerator(atPath: resolvedRoot.path) {
            while let rel = e.nextObject() as? String {
                let u = resolvedRoot.appendingPathComponent(rel)
                var st = stat()
                if lstat(u.path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG && rel != "SHA256SUMS" {
                    relPaths.append(rel)
                }
            }
        }
        relPaths.sort()
        var lines: [String] = []
        for r in relPaths {
            let h = try PayloadVerifier.sha256Hex(fileURL: resolvedRoot.appendingPathComponent(r))
            lines.append("\(h)  ./\(r)")
        }
        try (lines.joined(separator: "\n") + "\n").write(to: resolvedRoot.appendingPathComponent("SHA256SUMS"), atomically: true, encoding: .utf8)
    }

    // MARK: - Mock Command Runner

    private final class MockCommandRunner: CommandRunning, @unchecked Sendable {
        var productVersion = "26.7.2"
        var buildVersion = "25G332"
        var arch = "x86_64"
        var cpuFeatures = "FPU VME DE PSE"
        var csrutilStatus = "System Integrity Protection status: unknown (Custom Configuration).\n\tKext Signing: disabled\n\tFilesystem Protections: disabled"
        var optionsIoreg = "\"csr-active-config\" = <03080000>\n\"boot-args\" = \"garnet=1\""
        var pciGPUsIoreg = """
        +-o PEG0@1  <class IOPCIDevice>
            "vendor-id" = <02100000>
            "device-id" = <90750000>
            "class-code" = <00000300>
            "model" = "AMD Radeon RX 9060 XT"
            "assigned-addresses" = <00000000 00000000 00000000 00000004 00000000>
        """
        var garnetDriverIoreg = "\"ReBAR,BAR0,CurrentMB\" = 16384\n\"Garnet,DisplayPrimaryStatus\" = \"enabled\""
        var pciCompatIoreg = "\"GarnetPCIBarCompat,MatchMode\" = \"exact-uuid\"\n\"GarnetPCIBarCompat,IOPCIFamilyUUID\" = \"227771C6-ADC2-30C7-8408-263F723CF81C\""
        var kmutilLoaded = "org.garnet.driver\norg.garnet.accel\norg.garnet.pcibarcompat"
        var foreignAuxKexts: [(bundleID: String, bundlePath: String)] = []
        var failKmutilCreate = false
        var failGarnetTest = false
        var calls: [(exe: String, args: [String])] = []

        func run(_ executable: String, arguments: [String], environment: [String: String]?) -> CommandResult {
            calls.append((executable, arguments))
            if executable == "/usr/bin/tar" || executable == "/usr/sbin/chown" {
                return ProcessCommandRunner().run(executable, arguments: arguments, environment: environment)
            }
            if executable == "/usr/bin/sw_vers" {
                if arguments == ["-productVersion"] { return CommandResult(exitCode: 0, stdout: productVersion, stderr: "") }
                if arguments == ["-buildVersion"] { return CommandResult(exitCode: 0, stdout: buildVersion, stderr: "") }
            }
            if executable == "/usr/bin/uname" {
                return CommandResult(exitCode: 0, stdout: arch, stderr: "")
            }
            if executable == "/usr/sbin/sysctl" {
                return CommandResult(exitCode: 0, stdout: cpuFeatures, stderr: "")
            }
            if executable == "/usr/bin/csrutil" {
                return CommandResult(exitCode: 0, stdout: csrutilStatus, stderr: "")
            }
            if executable == "/usr/sbin/ioreg" {
                if arguments.contains("options") { return CommandResult(exitCode: 0, stdout: optionsIoreg, stderr: "") }
                if arguments.contains("IOPCIDevice") { return CommandResult(exitCode: 0, stdout: pciGPUsIoreg, stderr: "") }
                if arguments.contains("GarnetDriver") { return CommandResult(exitCode: 0, stdout: garnetDriverIoreg, stderr: "") }
                if arguments.contains("GarnetPCIBarCompat") { return CommandResult(exitCode: 0, stdout: pciCompatIoreg, stderr: "") }
            }
            if executable.hasSuffix("garnettest") {
                if failGarnetTest { return CommandResult(exitCode: 1, stdout: "", stderr: "mock fail") }
                if arguments.contains("vramstat") { return CommandResult(exitCode: 0, stdout: "vram=16384MB used=512MB", stderr: "") }
                return CommandResult(exitCode: 0, stdout: "ok\n", stderr: "")
            }
            if executable == "/usr/bin/kmutil" {
                if arguments.first == "showloaded" {
                    return CommandResult(exitCode: 0, stdout: kmutilLoaded, stderr: "")
                }
                if arguments.first == "create" {
                    if failKmutilCreate {
                        return CommandResult(exitCode: 1, stdout: "", stderr: "injected kmutil create error")
                    }
                    if let aIdx = arguments.firstIndex(of: "-A"), aIdx + 1 < arguments.count {
                        let outPath = arguments[aIdx + 1]
                        var pListEntries: [String] = []
                        for (idx, arg) in arguments.enumerated() where arg == "-p" && idx + 1 < arguments.count {
                            let kPath = arguments[idx + 1]
                            let bid: String
                            if kPath.hasSuffix("/Garnet.kext") { bid = "org.garnet.driver" }
                            else if kPath.hasSuffix("/GarnetAccel.kext") { bid = "org.garnet.accel" }
                            else if let fk = foreignAuxKexts.first(where: { kPath.hasSuffix($0.bundlePath) }) { bid = fk.bundleID }
                            else { bid = "org.unknown.\(idx)" }
                            pListEntries.append("<dict><key>CFBundleIdentifier</key><string>\(bid)</string><key>_PrelinkBundlePath</key><string>\(kPath)</string></dict>")
                        }
                        let xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>_PrelinkInfoDictionary</key><array>\(pListEntries.joined())</array></dict></plist>\n"
                        try? xml.write(toFile: outPath, atomically: true, encoding: .utf8)
                    }
                    return CommandResult(exitCode: 0, stdout: "", stderr: "")
                }
                if arguments.first == "inspect" {
                    if let aIdx = arguments.firstIndex(of: "-A"), aIdx + 1 < arguments.count {
                        let kcPath = arguments[aIdx + 1]
                        if let raw = try? String(contentsOfFile: kcPath, encoding: .utf8), raw.contains("_PrelinkInfoDictionary") {
                            return CommandResult(exitCode: 0, stdout: "No variant specified\n" + raw, stderr: "")
                        }
                        let entries = foreignAuxKexts.map {
                            "<dict><key>CFBundleIdentifier</key><string>\($0.bundleID)</string><key>_PrelinkBundlePath</key><string>\($0.bundlePath)</string></dict>"
                        }.joined()
                        let xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>_PrelinkInfoDictionary</key><array>\(entries)</array></dict></plist>\n"
                        return CommandResult(exitCode: 0, stdout: xml, stderr: "")
                    }
                }
            }
            if executable == "/usr/bin/log" {
                return CommandResult(exitCode: 0, stdout: "2026-11-23 Garnet: init ok for /Users/\(NSUserName())\n", stderr: "")
            }
            return CommandResult(exitCode: 0, stdout: "", stderr: "")
        }
    }

    // MARK: - OpenCore Config Plist Helper

    private static func makeOCPlist(
        exts: [(path: String, enabled: Bool)] = [
            ("Lilu.kext", true),
            ("VirtualSMC.kext", true),
            ("GarnetPCIBarCompat.kext", true)
        ],
        resizeApple: Int = -1,
        resizeUefi: Int = -1,
        increasePciBar: Bool = false,
        secureBoot: String = "Disabled",
        csrHex: String? = "03080000",
        bootArgs: String = "-v keepsyms=1 garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80 garnet-full-bar=1",
        deleteKeys: [String] = ["csr-active-config", "boot-args"]
    ) -> [String: Any] {
        let kEntries: [[String: Any]] = exts.map { item in
            let base = (item.path as NSString).lastPathComponent
            let stem = (base as NSString).deletingPathExtension
            return [
                "BundlePath": item.path,
                "Enabled": item.enabled,
                "ExecutablePath": "Contents/MacOS/\(stem)",
                "PlistPath": "Contents/Info.plist"
            ]
        }
        var nvramAddDict: [String: Any] = ["boot-args": bootArgs]
        if let ch = csrHex {
            var b = Data()
            var idx = ch.startIndex
            while ch.distance(from: idx, to: ch.endIndex) >= 2 {
                let nxt = ch.index(idx, offsetBy: 2)
                b.append(UInt8(ch[idx..<nxt], radix: 16) ?? 0)
                idx = nxt
            }
            nvramAddDict["csr-active-config"] = b
        }
        return [
            "Kernel": [
                "Add": kEntries,
                "Quirks": ["IncreasePciBarSize": increasePciBar]
            ],
            "Booter": ["Quirks": ["ResizeAppleGpuBars": resizeApple]],
            "UEFI": ["Quirks": ["ResizeGpuBars": resizeUefi]],
            "Misc": ["Security": ["SecureBootModel": secureBoot]],
            "NVRAM": [
                "Add": [OpenCoreValidator.nvramBootGUID: nvramAddDict],
                "Delete": [OpenCoreValidator.nvramBootGUID: deleteKeys]
            ]
        ]
    }

    private static func writePlist(_ dict: [String: Any], to url: URL) throws {
        let d = try PropertyListSerialization.data(fromPropertyList: dict, format: .xml, options: 0)
        try d.write(to: url)
    }

    // MARK: - Test Groups

    static func runPCIBarCompatTests() {
        let exact = makeIOPCIMachO(uuid: PCIBarCompatCheck.expected25G332UUID) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3712E)
        }
        let r1 = PCIBarCompatCheck.inspectMachOBytes(exact)
        check("PCIBar exact-uuid passes", r1.compatible && r1.mode == .exactUUID && r1.entryHdrOffsetHex == "0x3712e")

        // Different UUID & shifted prologue (+0x1000) -> structural pass
        let otherUUID: [UInt8] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01]
        let shifted = makeIOPCIMachO(uuid: otherUUID) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3812E)
        }
        let r2 = PCIBarCompatCheck.inspectMachOBytes(shifted)
        check("PCIBar structural shifted passes", r2.compatible && r2.mode == .structural && r2.entryHdrOffsetHex == "0x3812e")

        // Negative 1: all-zero UUID
        let zeroUUID = makeIOPCIMachO(uuid: [UInt8](repeating: 0, count: 16)) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3712E)
        }
        check("PCIBar all-zero UUID rejected", !PCIBarCompatCheck.inspectMachOBytes(zeroUUID).compatible)

        // Negative 2: corrupted struct anchor byte in structural mode
        let badAnchor = makeIOPCIMachO(uuid: otherUUID) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3812E)
            body[(0x3812E - rel) + 0xBF] ^= 0xFF
        }
        let r3 = PCIBarCompatCheck.inspectMachOBytes(badAnchor)
        check("PCIBar bad struct anchor rejected", !r3.compatible && r3.status == "struct-layout-mismatch")

        // Negative 3: duplicate prologue pattern
        let dupPrologue = makeIOPCIMachO(uuid: otherUUID) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3812E)
            for k in 0..<28 { body[0x100 + k] = body[(0x3812E - rel) + k] }
        }
        let r4 = PCIBarCompatCheck.inspectMachOBytes(dupPrologue)
        check("PCIBar duplicate prologue rejected", !r4.compatible && r4.status == "pattern-duplicate")
    }

    static func runMachOAndPreflightTests(tempRoot: URL) throws {
        let iopci = makeIOPCIMachO(uuid: PCIBarCompatCheck.expected25G332UUID) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3712E)
        }
        let fix = try buildSyntheticKCsAndPayload(root: tempRoot, iopciData: iopci)

        // Synthetic KC match
        let match = try MachOKCVerifier.matchVariant(variantsDirPayload: fix.payloadDir, bootKCPath: fix.bootKC.path, systemKCPath: fix.sysKC.path)
        check("MachOKC synthetic match", match.matched && match.matchedLayoutId == fix.layoutId && match.observations.count == 13)

        // Tamper fingerprint layoutId -> fails
        let tamperDir = tempRoot.appendingPathComponent("tamper-fp")
        try FileManager.default.copyItem(at: fix.payloadDir, to: tamperDir)
        let fpURL = tamperDir.appendingPathComponent("variants/\(fix.shortId)/layout-fingerprint.json")
        var fpText = try String(contentsOf: fpURL, encoding: .utf8)
        fpText = fpText.replacingOccurrences(of: "\"instanceSize\" : 128", with: "\"instanceSize\" : 136")
        try fpText.write(to: fpURL, atomically: true, encoding: .utf8)
        let mismatch = try MachOKCVerifier.matchVariant(variantsDirPayload: tamperDir, bootKCPath: fix.bootKC.path, systemKCPath: fix.sysKC.path)
        check("MachOKC tampered fingerprint rejected", !mismatch.matched)

        // Preflight ready
        let mock = MockCommandRunner()
        let env = HostEnvironment(sysRoot: tempRoot, bootKCPath: "/kcs/BootKernelExtensions.kc", systemKCPath: "/kcs/SystemKernelExtensions.kc", runner: mock, effectiveUID: 0)
        // Copy synthetic kcs under tempRoot/kcs already in fix.dir! Notice fix.dir IS tempRoot/kcs!
        let readyRep = PreflightChecker.run(env: env, payloadURL: fix.payloadDir)
        check("Preflight ready Navi 44", readyRep.overallStatus == .ready && readyRep.canInstall && readyRep.barRecommendation == .fullBar)

        // Preflight SIP required
        mock.csrutilStatus = "System Integrity Protection status: enabled."
        mock.optionsIoreg = "\"csr-active-config\" = <00000000>\n\"boot-args\" = \"garnet=1\""
        let sipRep = PreflightChecker.run(env: env, payloadURL: fix.payloadDir)
        check("Preflight sipSetupRequired", sipRep.overallStatus == .sipSetupRequired && !sipRep.canInstall)
        mock.csrutilStatus = "System Integrity Protection status: unknown (Custom Configuration).\n\tKext Signing: disabled\n\tFilesystem Protections: disabled"
        mock.optionsIoreg = "\"csr-active-config\" = <03080000>\n\"boot-args\" = \"garnet=1\""

        // Blocked: Navi 48 (1002:7550)
        mock.pciGPUsIoreg = "\"vendor-id\" = <02100000>\n\"device-id\" = <50750000>\n\"class-code\" = <00000300>\n\"model\" = \"Radeon RX 9070 XT\""
        check("Preflight blocks Navi 48", PreflightChecker.run(env: env, payloadURL: fix.payloadDir).overallStatus == .blocked)
        mock.pciGPUsIoreg = """
        +-o PEG0@1  <class IOPCIDevice>
            "vendor-id" = <02100000>
            "device-id" = <90750000>
            "class-code" = <00000300>
            "model" = "AMD Radeon RX 9060 XT"
        """

        // Blocked: macOS 15.x, arm64, VM
        mock.productVersion = "15.7.1"
        check("Preflight blocks macOS 15", PreflightChecker.run(env: env, payloadURL: fix.payloadDir).overallStatus == .blocked)
        mock.productVersion = "26.8.0"

        mock.arch = "arm64"
        check("Preflight blocks arm64", PreflightChecker.run(env: env, payloadURL: fix.payloadDir).overallStatus == .blocked)
        mock.arch = "x86_64"

        mock.cpuFeatures = "FPU VME VMM"
        check("Preflight blocks VM", PreflightChecker.run(env: env, payloadURL: fix.payloadDir).overallStatus == .blocked)
        mock.cpuFeatures = "FPU VME SSE"

        // Small-BAR fallback when PFM64Insufficient
        mock.pciCompatIoreg = "\"PFM64Insufficient\" = Yes"
        let smallRec = PreflightChecker.run(env: env, payloadURL: fix.payloadDir)
        check("Preflight recommends 256M fallback on PFM64Insufficient", smallRec.barRecommendation == .small256M)
        mock.pciCompatIoreg = "\"GarnetPCIBarCompat,MatchMode\" = \"exact-uuid\""

        // PayloadVerifier tests (valid, tamper, extra file, symlink, missing license, navi48 manifest)
        let pv = try PayloadVerifier.verify(payloadURL: fix.payloadDir, runner: mock, verifyCodeSignatures: false)
        check("PayloadVerifier passes valid release payload", pv.targetSilicon == "navi44" && pv.targetPCIIDs == "1002:7590")

        let badHashDir = tempRoot.appendingPathComponent("pv-tamper")
        try FileManager.default.copyItem(at: fix.payloadDir, to: badHashDir)
        try Data("tampered".utf8).write(to: badHashDir.appendingPathComponent("default-app-denylist.txt"))
        var threw = false
        do { _ = try PayloadVerifier.verify(payloadURL: badHashDir, runner: mock, verifyCodeSignatures: false) } catch { threw = true }
        check("PayloadVerifier rejects tampered SHA", threw)

        let extraDir = tempRoot.appendingPathComponent("pv-extra")
        try FileManager.default.copyItem(at: fix.payloadDir, to: extraDir)
        try Data("unlisted".utf8).write(to: extraDir.appendingPathComponent("rogue.txt"))
        threw = false
        do { _ = try PayloadVerifier.verify(payloadURL: extraDir, runner: mock, verifyCodeSignatures: false) } catch { threw = true }
        check("PayloadVerifier rejects unlisted file", threw)

        let symDir = tempRoot.appendingPathComponent("pv-sym")
        try FileManager.default.copyItem(at: fix.payloadDir, to: symDir)
        try FileManager.default.createSymbolicLink(atPath: symDir.appendingPathComponent("badlink").path, withDestinationPath: "/etc/passwd")
        threw = false
        do { _ = try PayloadVerifier.verify(payloadURL: symDir, runner: mock, verifyCodeSignatures: false) } catch { threw = true }
        check("PayloadVerifier rejects symlink", threw)

        let noLicDir = tempRoot.appendingPathComponent("pv-nolic")
        try FileManager.default.copyItem(at: fix.payloadDir, to: noLicDir)
        try Data().write(to: noLicDir.appendingPathComponent("LICENSE.amdgpu"))
        try regenerateSHA256SUMS(at: noLicDir)
        threw = false
        do { _ = try PayloadVerifier.verify(payloadURL: noLicDir, runner: mock, verifyCodeSignatures: false) } catch { threw = true }
        check("PayloadVerifier rejects empty LICENSE.amdgpu", threw)

        let n48Dir = tempRoot.appendingPathComponent("pv-n48")
        try FileManager.default.copyItem(at: fix.payloadDir, to: n48Dir)
        let mTxt = try String(contentsOf: n48Dir.appendingPathComponent("MANIFEST.txt"), encoding: .utf8)
            .replacingOccurrences(of: "CHIPS=navi44", with: "CHIPS=navi48")
        try mTxt.write(to: n48Dir.appendingPathComponent("MANIFEST.txt"), atomically: true, encoding: .utf8)
        try regenerateSHA256SUMS(at: n48Dir)
        threw = false
        do { _ = try PayloadVerifier.verify(payloadURL: n48Dir, runner: mock, verifyCodeSignatures: false) } catch { threw = true }
        check("PayloadVerifier rejects CHIPS=navi48", threw)
    }

    static func runOpenCoreValidatorTests(tempRoot: URL) throws {
        let ocDir = tempRoot.appendingPathComponent("oc")
        try FileManager.default.createDirectory(at: ocDir, withIntermediateDirectories: true)

        func verifyCase(_ name: String, _ plist: [String: Any], mode: OpenCoreMode, expectValid: Bool, expectWarn: Bool? = nil) throws {
            let u = ocDir.appendingPathComponent("\(name).plist")
            try writePlist(plist, to: u)
            let preSHA = try PayloadVerifier.sha256Hex(fileURL: u)
            let rep = try OpenCoreValidator.validate(configURL: u, mode: mode)
            let postSHA = try PayloadVerifier.sha256Hex(fileURL: u)
            check("OC read-only invariance (\(name))", preSHA == postSHA && rep.readOnlyVerified)
            check("OC valid state (\(name))", rep.valid == expectValid, detail: rep.findings.map { "[\($0.severity.rawValue)] \($0.keyPath): \($0.message)" }.joined(separator: " | "))
            if let ew = expectWarn {
                check("OC warn state (\(name))", (rep.warnCount > 0) == ew)
            }
        }

        check("OpenCoreMode rejects legacy '16g'", OpenCoreMode(rawValue: "16g") == nil)
        check("OpenCoreMode accepts 'full'", OpenCoreMode(rawValue: "full") == .full)
        check("BARRecommendation rejects legacy '16g-full-bar'", BARRecommendation(rawValue: "16g-full-bar") == nil)
        check("BARRecommendation accepts 'full-bar'", BARRecommendation(rawValue: "full-bar") == .fullBar)

        try verifyCase("valid-full", makeOCPlist(), mode: .full, expectValid: true, expectWarn: false)
        try verifyCase("valid-small", makeOCPlist(
            exts: [("Lilu.kext", true), ("GarnetPCIBarCompat.kext", false)],
            resizeApple: 0, resizeUefi: 0,
            bootArgs: "-v keepsyms=1 npci=0x2000 garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80"
        ), mode: .modeSmall, expectValid: true)

        // 10+ invalid config cases
        try verifyCase("bad-garnet-in-oc", makeOCPlist(exts: [("GarnetPCIBarCompat.kext", true), ("Garnet.kext", true)]), mode: .full, expectValid: false)
        try verifyCase("bad-accel-in-oc", makeOCPlist(exts: [("GarnetPCIBarCompat.kext", true), ("GarnetAccel.kext", true)]), mode: .full, expectValid: false)
        try verifyCase("bad-legacy-kext", makeOCPlist(exts: [("GarnetPCIBarCompat.kext", true), ("AMDRadeonRDNA4.kext", true)]), mode: .full, expectValid: false)
        try verifyCase("bad-missing-pcibar-16g", makeOCPlist(exts: [("Lilu.kext", true)]), mode: .full, expectValid: false)
        try verifyCase("bad-pcibar-enabled-small", makeOCPlist(
            exts: [("GarnetPCIBarCompat.kext", true)],
            resizeApple: 0, resizeUefi: 0,
            bootArgs: "-v npci=0x2000 garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80"
        ), mode: .modeSmall, expectValid: false)
        try verifyCase("bad-secureboot", makeOCPlist(secureBoot: "Default"), mode: .full, expectValid: false)
        try verifyCase("bad-csr-config", makeOCPlist(csrHex: "00000000"), mode: .full, expectValid: false)
        try verifyCase("bad-missing-delete", makeOCPlist(deleteKeys: ["boot-args"]), mode: .full, expectValid: false)
        try verifyCase("warn-missing-apps-all", makeOCPlist(bootArgs: "garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 amfi=0x80 garnet-full-bar=1"), mode: .full, expectValid: true, expectWarn: true)
        try verifyCase("bad-missing-required-boot-arg", makeOCPlist(bootArgs: "garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80 garnet-full-bar=1"), mode: .full, expectValid: false)
        try verifyCase("bad-npci-with-16g", makeOCPlist(bootArgs: "npci=0x2000 garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80 garnet-full-bar=1"), mode: .full, expectValid: false)
        try verifyCase("bad-resize-apple-16g", makeOCPlist(resizeApple: 0), mode: .full, expectValid: false)
        try verifyCase("bad-increase-pci-bar", makeOCPlist(increasePciBar: true), mode: .full, expectValid: false)
    }

    static func runOpenCoreConfigureTests(tempRoot: URL) throws {
        let fm = FileManager.default
        let payloadDir = tempRoot.appendingPathComponent("dummy-oc-payload")
        let compatSrc = payloadDir.appendingPathComponent("optional/GarnetPCIBarCompat.kext/Contents/MacOS")
        try fm.createDirectory(at: compatSrc, withIntermediateDirectories: true)
        try Data("<plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>org.garnet.pcibarcompat</string></dict></plist>".utf8)
            .write(to: payloadDir.appendingPathComponent("optional/GarnetPCIBarCompat.kext/Contents/Info.plist"))
        try Data("dummy-pcibar".utf8).write(to: compatSrc.appendingPathComponent("GarnetPCIBarCompat"))

        // 1. 16G directory auto-configure
        let efi16 = tempRoot.appendingPathComponent("efi16")
        let ocDir = efi16.appendingPathComponent("EFI/OC")
        try fm.createDirectory(at: ocDir, withIntermediateDirectories: true)
        let cfgURL = ocDir.appendingPathComponent("config.plist")
        let initialPlist = makeOCPlist(
            exts: [("Lilu.kext", true), ("VirtualSMC.kext", true), ("Garnet.kext", true)],
            resizeApple: 0,
            resizeUefi: 0,
            increasePciBar: true,
            secureBoot: "Default",
            csrHex: "00000000",
            bootArgs: "-v alcid=11 -radvesa npci=0x2000 rdna4-foo=1",
            deleteKeys: []
        )
        try writePlist(initialPlist, to: cfgURL)
        let origSHA = try PayloadVerifier.sha256Hex(fileURL: cfgURL)

        let res16 = try OpenCoreValidator.configure(targetURL: efi16, mode: .full, payloadURL: payloadDir)
        let backupSHA = try PayloadVerifier.sha256Hex(fileURL: URL(fileURLWithPath: res16.backupPath))
        check("OC configure 16g backup exists with original SHA", fm.fileExists(atPath: res16.backupPath) && backupSHA == origSHA)
        check("OC configure 16g copied GarnetPCIBarCompat.kext", fm.fileExists(atPath: ocDir.appendingPathComponent("Kexts/GarnetPCIBarCompat.kext/Contents/MacOS/GarnetPCIBarCompat").path))
        check("OC configure 16g valid with 0 warnings", res16.report.valid && res16.report.warnCount == 0)

        let configuredData = try Data(contentsOf: cfgURL)
        let configuredRoot = try PropertyListSerialization.propertyList(from: configuredData, options: [], format: nil) as? [String: Any] ?? [:]
        let kernelAdd = ((configuredRoot["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]]) ?? []
        let compatEntries = kernelAdd.filter { (($0["BundlePath"] as? String) ?? "") == "GarnetPCIBarCompat.kext" }
        let garnetEntry = kernelAdd.first { (($0["BundlePath"] as? String) ?? "") == "Garnet.kext" }
        let liluEnabled = (kernelAdd.first { (($0["BundlePath"] as? String) ?? "") == "Lilu.kext" }?["Enabled"] as? Bool) ?? false
        let smcEnabled = (kernelAdd.first { (($0["BundlePath"] as? String) ?? "") == "VirtualSMC.kext" }?["Enabled"] as? Bool) ?? false
        let nvramAdd = (((configuredRoot["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[OpenCoreValidator.nvramBootGUID] as? [String: Any]) ?? [:]
        let argsTokens = ((nvramAdd["boot-args"] as? String) ?? "").split(whereSeparator: { $0.isWhitespace }).map(String.init)
        check(
            "OC configure 16g preserves user flags/kexts and disables Garnet.kext",
            compatEntries.count == 1 && liluEnabled && smcEnabled && ((garnetEntry?["Enabled"] as? Bool) == false)
                && argsTokens.contains("-v") && argsTokens.contains("alcid=11")
                && !argsTokens.contains("-radvesa") && !argsTokens.contains("npci=0x2000") && !argsTokens.contains("rdna4-foo=1")
        )

        // 2. Idempotency
        let res16Again = try OpenCoreValidator.configure(targetURL: cfgURL, mode: .full, payloadURL: payloadDir)
        let againRoot = try PropertyListSerialization.propertyList(from: Data(contentsOf: cfgURL), options: [], format: nil) as? [String: Any] ?? [:]
        let againAdd = ((againRoot["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]]) ?? []
        let againCompatCount = againAdd.filter { (($0["BundlePath"] as? String) ?? "") == "GarnetPCIBarCompat.kext" }.count
        let againArgs = ((((againRoot["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[OpenCoreValidator.nvramBootGUID] as? [String: Any])?["boot-args"] as? String ?? "").split(whereSeparator: { $0.isWhitespace }).map(String.init)
        check("OC configure 16g is idempotent", res16Again.report.valid && againCompatCount == 1 && Set(againArgs).count == againArgs.count)

        // 3. Switch to 256 MiB Small-BAR
        let resSmall = try OpenCoreValidator.configure(targetURL: cfgURL, mode: .modeSmall, payloadURL: nil)
        let smallRoot = try PropertyListSerialization.propertyList(from: Data(contentsOf: cfgURL), options: [], format: nil) as? [String: Any] ?? [:]
        let smallAdd = ((smallRoot["Kernel"] as? [String: Any])?["Add"] as? [[String: Any]]) ?? []
        let smallCompatEnabled = (smallAdd.first { (($0["BundlePath"] as? String) ?? "") == "GarnetPCIBarCompat.kext" }?["Enabled"] as? Bool) ?? true
        let smallArgs = ((((smallRoot["NVRAM"] as? [String: Any])?["Add"] as? [String: Any])?[OpenCoreValidator.nvramBootGUID] as? [String: Any])?["boot-args"] as? String ?? "").split(whereSeparator: { $0.isWhitespace }).map(String.init)
        let smallResizeApple = ((smallRoot["Booter"] as? [String: Any])?["Quirks"] as? [String: Any])?["ResizeAppleGpuBars"] as? Int
        let smallResizeUefi = ((smallRoot["UEFI"] as? [String: Any])?["Quirks"] as? [String: Any])?["ResizeGpuBars"] as? Int
        check(
            "OC configure switches cleanly to small-bar",
            resSmall.report.valid && !smallCompatEnabled && !smallArgs.contains("garnet-full-bar=1")
                && smallArgs.contains("npci=0x2000") && smallResizeApple == 0 && smallResizeUefi == 0
        )

        // 4. Symlink refusal
        let symTarget = tempRoot.appendingPathComponent("real-target.plist")
        try writePlist(makeOCPlist(), to: symTarget)
        let targetPreSHA = try PayloadVerifier.sha256Hex(fileURL: symTarget)
        let symLink = tempRoot.appendingPathComponent("link-config.plist")
        try fm.createSymbolicLink(at: symLink, withDestinationURL: symTarget)
        var refusedSym = false
        do { _ = try OpenCoreValidator.configure(targetURL: symLink, mode: .full, payloadURL: payloadDir) } catch { refusedSym = true }
        let targetPostSHA = try PayloadVerifier.sha256Hex(fileURL: symTarget)
        check("OC configure refuses symlink and leaves target untouched", refusedSym && targetPreSHA == targetPostSHA)
    }

    static func runDenylistInstallerDiagTests(tempRoot: URL) throws {
        let fm = FileManager.default
        let sysRoot = tempRoot.appendingPathComponent("sysroot")
        try fm.createDirectory(at: sysRoot, withIntermediateDirectories: true)

        // Reuse already-created synthetic payload & kcs by linking or building fresh in subdir
        let iopci = makeIOPCIMachO(uuid: PCIBarCompatCheck.expected25G332UUID) { body, rel in
            populateValidIOPCIFunc(bytes: &body, textRel: rel, entryHdrOff: 0x3712E)
        }
        let fix = try buildSyntheticKCsAndPayload(root: sysRoot, iopciData: iopci)
        let mock = MockCommandRunner()

        // Create a foreign kext & garnettest binary in sysRoot
        let foreignPath = "/Library/Extensions/VirtualSMC.kext"
        try fm.createDirectory(at: sysRoot.appendingPathComponent("Library/Extensions/VirtualSMC.kext"), withIntermediateDirectories: true)
        mock.foreignAuxKexts = [("as.vit9696.VirtualSMC", foreignPath)]
        try fm.createDirectory(at: sysRoot.appendingPathComponent("Library/KernelCollections"), withIntermediateDirectories: true)
        try Data("preexisting-aux".utf8).write(to: sysRoot.appendingPathComponent("Library/KernelCollections/AuxiliaryKernelExtensions.kc"))

        let env = HostEnvironment(sysRoot: sysRoot, bootKCPath: "/kcs/BootKernelExtensions.kc", systemKCPath: "/kcs/SystemKernelExtensions.kc", runner: mock, effectiveUID: 501)

        // Dry run install
        let dryPlan = try SystemDiskInstaller.install(
            payloadURL: fix.payloadDir,
            options: InstallOptions(installVulkanICD: false, dryRun: true, verifyCodeSignatures: false, allowNonRootSandbox: true),
            env: env
        )
        check("Installer dryRun=true does not write Garnet.kext", dryPlan.dryRun && !fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Extensions/Garnet.kext").path))
        check("Installer dryRun preserves foreign kext list", dryPlan.preservedForeignAuxKexts.count == 1)

        // Real sandbox install (Vulkan OFF by default)
        let instRes = try SystemDiskInstaller.install(
            payloadURL: fix.payloadDir,
            options: InstallOptions(installVulkanICD: false, dryRun: false, verifyCodeSignatures: false, allowNonRootSandbox: true),
            env: env
        )
        check("Installer executed", instRes.executed && !instRes.vulkanICDInstalled)
        check("Garnet.kext & GarnetAccel.kext installed", fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Extensions/Garnet.kext/Contents/Info.plist").path) && fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Extensions/GarnetAccel.kext/Contents/Info.plist").path))
        check("GarnetPCIBarCompat.kext NEVER placed in /Library/Extensions", !fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Extensions/GarnetPCIBarCompat.kext").path))
        check("Vulkan ICD omitted by default", !fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Vulkan/icd.d/radeon_icd.x86_64.json").path))
        check("GarnetMetal.bundle extracted", fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/libvulkan_radeon.dylib").path))

        // Denylist persistence + upgrade test
        let mockApp = tempRoot.appendingPathComponent("MockHeavyApp.app")
        try fm.createDirectory(at: mockApp.appendingPathComponent("Contents"), withIntermediateDirectories: true)
        try """
        <?xml version="1.0" encoding="UTF-8"?>
        <plist version="1.0"><dict>
        <key>CFBundleIdentifier</key><string>com.example.VeryLongAppIdentifierBeyondSixteenChars</string>
        <key>CFBundleExecutable</key><string>HeavyProc</string>
        </dict></plist>
        """.write(to: mockApp.appendingPathComponent("Contents/Info.plist"), atomically: true, encoding: .utf8)

        let (meta, _) = try DenylistManager.addAppBundle(at: mockApp, env: env)
        check("Denylist extracted long bundle id", meta.bundleIdentifier == "com.example.VeryLongAppIdentifierBeyondSixteenChars")
        check("Denylist called live garnettest appdeny add", mock.calls.contains(where: { $0.exe.hasSuffix("garnettest") && $0.args == ["accel", "appdeny", "add", "HeavyProc"] }))

        // Refuses WindowServer & control chars
        var refusedWS = false
        do { _ = try DenylistManager.add(kind: .proc, value: "WindowServer", env: env) } catch DenylistError.protectedTarget { refusedWS = true } catch {}
        check("Denylist refuses WindowServer", refusedWS)
        var refusedNL = false
        do { _ = try DenylistManager.add(kind: .proc, value: "Bad\nName", env: env) } catch DenylistError.invalidEntry { refusedNL = true } catch {}
        check("Denylist refuses newline in proc name", refusedNL)

        // Upgrade preserving custom denylist & enabling optional Vulkan ICD
        let upgRes = try SystemDiskInstaller.install(
            payloadURL: fix.payloadDir,
            options: InstallOptions(installVulkanICD: true, dryRun: false, verifyCodeSignatures: false, allowNonRootSandbox: true),
            env: env
        )
        check("Upgrade preserves denylist", upgRes.denylistPreservedOnUpgrade && upgRes.vulkanICDInstalled)
        let loadedAfterUpg = try DenylistManager.load(env: env)
        check("Custom bundle entry survived upgrade", loadedAfterUpg.contains(where: { $0.kind == .bundle && $0.value == "com.example.VeryLongAppIdentifierBeyondSixteenChars" }))
        check("Vulkan ICD & radv-app installed when enabled", fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Vulkan/icd.d/radeon_icd.x86_64.json").path) && fm.isExecutableFile(atPath: sysRoot.appendingPathComponent("usr/local/bin/radv-app").path))

        // Rollback test on injected kmutil failure (mark manifest temporarily to confirm restore!)
        let manifestSysURL = sysRoot.appendingPathComponent("Library/Application Support/RDNA4/native/MANIFEST.txt")
        let sentinelText = try String(contentsOf: manifestSysURL, encoding: .utf8) + "# SENTINEL_PRE_ROLLBACK\n"
        try sentinelText.write(to: manifestSysURL, atomically: true, encoding: .utf8)
        mock.failKmutilCreate = true
        var rolledBackErr = false
        do {
            _ = try SystemDiskInstaller.install(
                payloadURL: fix.payloadDir,
                options: InstallOptions(installVulkanICD: true, dryRun: false, verifyCodeSignatures: false, allowNonRootSandbox: true),
                env: env
            )
        } catch SystemInstallError.kmutilFailed {
            rolledBackErr = true
        }
        mock.failKmutilCreate = false
        let postRollbackText = try String(contentsOf: manifestSysURL, encoding: .utf8)
        check("Transactional rollback triggered and restored MANIFEST", rolledBackErr && postRollbackText.contains("# SENTINEL_PRE_ROLLBACK"))

        // Export GarnetPCIBarCompat
        let expDir = tempRoot.appendingPathComponent("exported-oc")
        let expKext = try SystemDiskInstaller.exportPCIBarCompat(fromPayload: fix.payloadDir, toDestination: expDir)
        check("Exported GarnetPCIBarCompat.kext", fm.fileExists(atPath: expKext.appendingPathComponent("Contents/Info.plist").path))

        // Status & sanitized diagnostics export
        let st = DiagnosticsManager.collectStatus(env: env)
        check("Status parses driver/BAR/compat/denylist", st.driverLoaded && st.accelLoaded && st.pciBarCompatLoaded && st.bar0CurrentMB == 16384 && st.iopciCompatMatchMode == "exact-uuid" && st.vulkanInstalled && st.denylistCount >= 2)
        mock.garnetDriverIoreg = "\"ReBAR,BAR0,CurrentMB\" = 8192\n\"Garnet,DisplayPrimaryStatus\" = \"enabled\""
        let st8g = DiagnosticsManager.collectStatus(env: env)
        check("Status parses 8 GiB BAR0 CurrentMB", st8g.bar0CurrentMB == 8192)
        mock.garnetDriverIoreg = "\"ReBAR,BAR0,CurrentMB\" = 16384\n\"Garnet,DisplayPrimaryStatus\" = \"enabled\""
        let diagOut = tempRoot.appendingPathComponent("diag.tar.gz")
        _ = try DiagnosticsManager.exportSanitizedDiagnostics(toArchive: diagOut, env: env, payloadURL: fix.payloadDir)
        check("Diagnostics archive created", fm.fileExists(atPath: diagOut.path))
        let scrubbed = DiagnosticsManager.sanitizeText("IOPlatformSerialNumber = \"C02FAKE12345\"\npath=/Users/\(NSUserName())/foo\n")
        check("Sanitizer strips serial & username", !scrubbed.contains("C02FAKE12345") && scrubbed.contains("REDACTED HARDWARE IDENTIFIER") && !scrubbed.contains("/Users/\(NSUserName())/"))

        // Uninstall preserving foreign kext & denylist
        let unRes = try SystemDiskInstaller.uninstall(
            options: UninstallOptions(removeDenylist: false, dryRun: false, allowNonRootSandbox: true),
            env: env
        )
        check("Uninstall removed Garnet.kext & preserved foreign AuxKC", unRes.executed && !unRes.auxKCRemoved && unRes.preservedForeignAuxKexts.count == 1 && !fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/Extensions/Garnet.kext").path))
        check("Uninstall preserved custom denylist", fm.fileExists(atPath: sysRoot.appendingPathComponent(DenylistManager.denylistPath.dropFirst().description).path))

        // Uninstall with 0 foreign kexts removes AuxKC
        mock.foreignAuxKexts = []
        try Data("empty-aux".utf8).write(to: sysRoot.appendingPathComponent("Library/KernelCollections/AuxiliaryKernelExtensions.kc"))
        let unResEmpty = try SystemDiskInstaller.uninstall(
            options: UninstallOptions(removeDenylist: true, dryRun: false, allowNonRootSandbox: true),
            env: env
        )
        check("Uninstall removes AuxKC when zero foreign kexts", unResEmpty.auxKCRemoved && !fm.fileExists(atPath: sysRoot.appendingPathComponent("Library/KernelCollections/AuxiliaryKernelExtensions.kc").path))
        check("No ESP/diskutil calls emitted", !mock.calls.contains(where: { $0.exe.contains("diskutil") }))

        let nulPaddedXML = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>_PrelinkInfoDictionary</key><array><dict><key>CFBundleIdentifier</key><string>com.apple.driver.AppleMobileDevice</string><key>_PrelinkBundlePath</key><string>/Library/Apple/System/Library/Extensions/AppleMobileDevice.kext</string></dict></array></dict></plist>" + String(repeating: "\0", count: 512)
        let parsedNul = try SystemDiskInstaller.parsePrelinkInfoEntries(from: nulPaddedXML)
        check("Prelink plist parser handles trailing NUL padding", parsedNul.count == 1 && parsedNul[0].bundleID == "com.apple.driver.AppleMobileDevice")
    }

    static func runOptionalReal25G332Checks() throws {
        let envMap = ProcessInfo.processInfo.environment
        guard let kcDir = envMap["GARNET_REAL_KC_DIR"], !kcDir.isEmpty,
              let payloadPath = envMap["GARNET_TEST_PAYLOAD"], !payloadPath.isEmpty else {
            return
        }
        let bootKC = URL(fileURLWithPath: kcDir).appendingPathComponent("BootKernelExtensions.kc").path
        let sysKC = URL(fileURLWithPath: kcDir).appendingPathComponent("SystemKernelExtensions.kc").path
        let payloadURL = URL(fileURLWithPath: payloadPath)

        print("Running real 25G332 KC verification against \(kcDir) & \(payloadPath)...")
        let match = try MachOKCVerifier.matchVariant(variantsDirPayload: payloadURL, bootKCPath: bootKC, systemKCPath: sysKC)
        check("Real 25G332 KC layout matches payload variant", match.matched && match.observations.count == 13, detail: match.summary)

        let barCompat = PCIBarCompatCheck.checkBootKC(path: bootKC)
        check("Real 25G332 BootKC IOPCIFamily exact-uuid matches", barCompat.compatible && barCompat.mode == .exactUUID && barCompat.entryHdrOffsetHex == "0x3712e", detail: "\(barCompat.status) \(barCompat.iopciFamilyUUID ?? "nil")")
    }

    static func main() {
        let fm = FileManager.default
        let tempRoot = fm.temporaryDirectory.appendingPathComponent("garnet-core-tests-\(UUID().uuidString)")
        do {
            try fm.createDirectory(at: tempRoot, withIntermediateDirectories: true)
            defer { try? fm.removeItem(at: tempRoot) }

            runPCIBarCompatTests()
            try runMachOAndPreflightTests(tempRoot: tempRoot)
            try runOpenCoreValidatorTests(tempRoot: tempRoot)
            try runOpenCoreConfigureTests(tempRoot: tempRoot)
            try runDenylistInstallerDiagTests(tempRoot: tempRoot)
            try runOptionalReal25G332Checks()
        } catch {
            failed += 1
            fputs("UNCAUGHT TEST EXCEPTION: \(error)\n", stderr)
        }

        print("GarnetCoreTests: \(passed) checks passed, \(failed) failed")
        Foundation.exit(failed == 0 ? 0 : 1)
    }
}
