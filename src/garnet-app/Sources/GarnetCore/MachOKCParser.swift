import Foundation
import CryptoKit

public struct FPInheritedBinding: Codable, Equatable, Sendable {
    public let slot: Int
    public let syms: [String]
}

public struct FPClassEntry: Codable, Equatable, Sendable {
    public let leaf: String
    public let parent: String
    public let instanceSize: Int
    public let nslots: Int
    public let overrideSlots: [Int]
    public let inheritedBindings: [FPInheritedBinding]
}

public struct LayoutFingerprint: Codable, Equatable, Sendable {
    public let schema: String
    public let arch: String
    public let expectClasses: Int
    public let expectSlots: Int
    public let layoutId: String
    public let shortId: String
    public let classes: [FPClassEntry]
}

public struct VariantIndexEntry: Codable, Equatable, Sendable {
    public let layoutId: String
    public let shortId: String
    public let accelBundleRelPath: String
    public let fingerprintRelPath: String
    public let verifiedBuilds: [String]
}

public struct VariantsIndex: Codable, Equatable, Sendable {
    public let schema: String
    public let defaultVariant: String
    public let variants: [VariantIndexEntry]
}

public struct LocalClassObservation: Codable, Equatable, Sendable {
    public let leaf: String
    public let parent: String
    public let expectedSize: Int
    public let actualSize: Int?
    public let expectedSlots: Int
    public let actualSlots: Int?
    public let mismatchedSlots: [String]
}

public struct LayoutMatchResult: Codable, Equatable, Sendable {
    public let matched: Bool
    public let matchedLayoutId: String?
    public let matchedShortId: String?
    public let matchedVariantRelPath: String?
    public let summary: String
    public let observations: [LocalClassObservation]
}

public enum KCParserError: Error, CustomStringConvertible {
    case fileOpenFailed(String)
    case truncatedRead(String)
    case invalidMachO(String)
    case missingFileset(String)

    public var description: String {
        switch self {
        case .fileOpenFailed(let m): return "Cannot open Mach-O/KC: \(m)"
        case .truncatedRead(let m): return "Truncated Mach-O/KC read: \(m)"
        case .invalidMachO(let m): return "Invalid Mach-O/KC structure: \(m)"
        case .missingFileset(let m): return "Missing fileset entry: \(m)"
        }
    }
}

public struct MachOSegment64: Sendable {
    public let name: String
    public let vmaddr: UInt64
    public let vmsize: UInt64
    public let fileoff: UInt64
    public let filesize: UInt64
}

public struct MachOFilesetEntry: Sendable {
    public let identifier: String
    public let vmaddr: UInt64
    public let fileoff: UInt64
}

public final class StreamingKCImage {
    public static let bootKernelChainedBase: UInt64 = 0xFFFFFF8000100000

    private let handle: FileHandle
    private let fileSize: UInt64
    public let path: String
    public let segments: [MachOSegment64]
    public let filesets: [MachOFilesetEntry]

    private var sortedAddrs: [UInt64] = []
    private var addrToSyms: [UInt64: [String]] = [:]
    private var symToAddr: [String: UInt64] = [:]
    private var symtabsLoaded = false

    public init(path: String) throws {
        self.path = path
        guard let fh = FileHandle(forReadingAtPath: path) else {
            throw KCParserError.fileOpenFailed(path)
        }
        self.handle = fh
        self.fileSize = fh.seekToEndOfFile()
        fh.seek(toFileOffset: 0)

        let hdr = try StreamingKCImage.readExact(fh, at: 0, count: 32, fileSize: self.fileSize, label: "mach_header_64")
        let magic = hdr.leUInt32(0)
        let cpu = hdr.leUInt32(4)
        let ftype = hdr.leUInt32(12)
        let ncmds = hdr.leUInt32(16)
        let szcmds = hdr.leUInt32(20)
        guard magic == 0xFEEDFACF, cpu == 0x01000007, (ftype == 12 || ftype == 11) else {
            throw KCParserError.invalidMachO("\(path): magic=\(String(format: "%#x", magic)) cpu=\(String(format: "%#x", cpu)) filetype=\(ftype)")
        }
        guard ncmds > 0, szcmds >= 8, UInt64(32) + UInt64(szcmds) <= self.fileSize else {
            throw KCParserError.invalidMachO("\(path): invalid load commands")
        }

        let cmds = try StreamingKCImage.readExact(fh, at: 32, count: Int(szcmds), fileSize: self.fileSize, label: "load_commands")
        var segs: [MachOSegment64] = []
        var fs: [MachOFilesetEntry] = []
        var off = 0
        for _ in 0..<ncmds {
            guard off + 8 <= cmds.count else { throw KCParserError.invalidMachO("load command OOB") }
            let cmd = cmds.leUInt32(off)
            let cmdsize = Int(cmds.leUInt32(off + 4))
            guard cmdsize >= 8, off + cmdsize <= cmds.count else { throw KCParserError.invalidMachO("bad cmdsize") }
            if cmd == 0x19 && cmdsize >= 72 {
                let name = cmds.fixedCString(at: off + 8, maxLen: 16)
                let vm = cmds.leUInt64(off + 24)
                let vs = cmds.leUInt64(off + 32)
                let fo = cmds.leUInt64(off + 40)
                let fsz = cmds.leUInt64(off + 48)
                segs.append(MachOSegment64(name: name, vmaddr: vm, vmsize: vs, fileoff: fo, filesize: fsz))
            } else if cmd == 0x80000035 && cmdsize >= 32 {
                let vm = cmds.leUInt64(off + 8)
                let fo = cmds.leUInt64(off + 16)
                let idOff = Int(cmds.leUInt32(off + 24))
                if idOff >= 24 && off + idOff < off + cmdsize {
                    let ident = cmds.fixedCString(at: off + idOff, maxLen: off + cmdsize - (off + idOff))
                    fs.append(MachOFilesetEntry(identifier: ident, vmaddr: vm, fileoff: fo))
                }
            }
            off += cmdsize
        }
        self.segments = segs
        self.filesets = fs
    }

    deinit {
        try? handle.close()
    }

    private static func readExact(_ fh: FileHandle, at offset: UInt64, count: Int, fileSize: UInt64, label: String) throws -> Data {
        guard count >= 0, offset <= fileSize, UInt64(count) <= fileSize - offset else {
            throw KCParserError.truncatedRead("\(label) @ \(offset)+\(count) > \(fileSize)")
        }
        if count == 0 { return Data() }
        try fh.seek(toOffset: offset)
        guard let data = try fh.read(upToCount: count), data.count == count else {
            throw KCParserError.truncatedRead("\(label) short read")
        }
        return data
    }

    public func readAtFileOffset(_ offset: UInt64, count: Int) throws -> Data {
        try StreamingKCImage.readExact(handle, at: offset, count: count, fileSize: fileSize, label: "slice")
    }

    public func fileOffset(forVMAddr va: UInt64) -> UInt64? {
        for seg in segments where seg.filesize > 0 {
            if va >= seg.vmaddr && (va - seg.vmaddr) < seg.filesize {
                return seg.fileoff + (va - seg.vmaddr)
            }
        }
        return nil
    }

    public func readBytes(atVMAddr va: UInt64, count: Int) -> Data? {
        guard let fo = fileOffset(forVMAddr: va) else { return nil }
        return try? readAtFileOffset(fo, count: count)
    }

    public func readUInt64(atVMAddr va: UInt64) -> UInt64? {
        guard let d = readBytes(atVMAddr: va, count: 8), d.count == 8 else { return nil }
        return d.leUInt64(0)
    }

    public func readCString(atVMAddr va: UInt64, maxLen: Int = 128) -> String? {
        guard let fo = fileOffset(forVMAddr: va) else { return nil }
        let avail = Int(min(UInt64(maxLen), fileSize - min(fileSize, fo)))
        guard avail > 0, let d = try? readAtFileOffset(fo, count: avail) else { return nil }
        return d.fixedCString(at: 0, maxLen: d.count)
    }

    public func filesetEntry(identifier: String) -> MachOFilesetEntry? {
        filesets.first(where: { $0.identifier == identifier })
    }

    public func ensureSymtabsLoaded(filesetFilter: Set<String>? = nil) throws {
        if symtabsLoaded { return }
        var strCache: [String: Data] = [:]
        var aMap: [UInt64: [String]] = [:]
        var nMap: [String: UInt64] = [:]

        // Also index top-level LC_SYMTAB if present (standalone Mach-O fixtures in tests use this).
        let topHdr = try readAtFileOffset(0, count: 32)
        let topNCmds = topHdr.leUInt32(16)
        let topSzCmds = Int(topHdr.leUInt32(20))
        if topSzCmds > 0 {
            let topCmds = try readAtFileOffset(32, count: topSzCmds)
            try indexSymtabs(fromCommands: topCmds, ncmds: topNCmds, strCache: &strCache, addrToSyms: &aMap, symToAddr: &nMap)
        }

        for fs in filesets {
            if let filter = filesetFilter, !filter.contains(fs.identifier) { continue }
            let fhdr = try readAtFileOffset(fs.fileoff, count: 32)
            let magic = fhdr.leUInt32(0)
            guard magic == 0xFEEDFACF else { continue }
            let ncmds = fhdr.leUInt32(16)
            let szcmds = Int(fhdr.leUInt32(20))
            guard szcmds >= 8 else { continue }
            let fcmds = try readAtFileOffset(fs.fileoff + 32, count: szcmds)
            try indexSymtabs(fromCommands: fcmds, ncmds: ncmds, strCache: &strCache, addrToSyms: &aMap, symToAddr: &nMap)
        }

        self.addrToSyms = aMap
        self.symToAddr = nMap
        self.sortedAddrs = aMap.keys.sorted()
        self.symtabsLoaded = true
    }

    private func indexSymtabs(
        fromCommands cmds: Data,
        ncmds: UInt32,
        strCache: inout [String: Data],
        addrToSyms: inout [UInt64: [String]],
        symToAddr: inout [String: UInt64]
    ) throws {
        var off = 0
        for _ in 0..<ncmds {
            guard off + 8 <= cmds.count else { break }
            let cmd = cmds.leUInt32(off)
            let cmdsize = Int(cmds.leUInt32(off + 4))
            guard cmdsize >= 8, off + cmdsize <= cmds.count else { break }
            if cmd == 0x2 && cmdsize >= 24 {
                let symoff = UInt64(cmds.leUInt32(off + 8))
                let nsyms = Int(cmds.leUInt32(off + 12))
                let stroff = UInt64(cmds.leUInt32(off + 16))
                let strsize = Int(cmds.leUInt32(off + 20))
                if nsyms > 0 && strsize > 0 {
                    let cacheKey = "\(stroff):\(strsize)"
                    let strData: Data
                    if let cached = strCache[cacheKey] {
                        strData = cached
                    } else {
                        let loaded = try readAtFileOffset(stroff, count: strsize)
                        strCache[cacheKey] = loaded
                        strData = loaded
                    }
                    let symData = try readAtFileOffset(symoff, count: nsyms * 16)
                    symData.withUnsafeBytes { sBuf in
                        strData.withUnsafeBytes { stBuf in
                            guard let sp = sBuf.baseAddress?.assumingMemoryBound(to: UInt8.self),
                                  let tp = stBuf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
                            for idx in 0..<nsyms {
                                let base = idx * 16
                                let strx = Int(sp.loadUnalignedLE32(at: base))
                                let nType = sp[base + 4]
                                if (nType & 0xE0) != 0 || (nType & 0x0E) == 0 { continue }
                                if strx >= strsize { continue }
                                var end = strx
                                while end < strsize && tp[end] != 0 { end += 1 }
                                let nm = String(decoding: UnsafeBufferPointer(start: tp + strx, count: end - strx), as: UTF8.self)
                                let nValue = sp.loadUnalignedLE64(at: base + 8)
                                addrToSyms[nValue, default: []].append(nm)
                                if symToAddr[nm] == nil { symToAddr[nm] = nValue }
                            }
                        }
                    }
                }
            }
            off += cmdsize
        }
    }

    public func symbolAddress(_ name: String) -> UInt64? {
        symToAddr[name]
    }

    public func symbols(at addr: UInt64) -> [String] {
        addrToSyms[addr] ?? []
    }

    public func nextSymbolAddress(after addr: UInt64) -> UInt64? {
        var lo = 0
        var hi = sortedAddrs.count
        while lo < hi {
            let mid = (lo + hi) / 2
            if sortedAddrs[mid] <= addr { lo = mid + 1 } else { hi = mid }
        }
        return lo < sortedAddrs.count ? sortedAddrs[lo] : nil
    }
}

public enum MachOKCVerifier {
    public static func mangleClass(_ name: String) -> String {
        "\(name.utf8.count)\(name)"
    }

    public static func decodeChainedFixup(_ raw: UInt64, defaultLevel: Int = 0) -> (level: Int, va: UInt64) {
        if raw >= (1 as UInt64) << 63 {
            return (0, raw)
        }
        let lvl = Int((raw >> 30) & 3)
        let off = raw & 0x3FFFFFFF
        if lvl == 0 {
            return (0, StreamingKCImage.bootKernelChainedBase + off)
        }
        return (lvl, off)
    }

    private static func isCIdentifier(_ s: String) -> Bool {
        guard let first = s.first else { return false }
        guard first == "_" || first.isLetter else { return false }
        return s.allSatisfy { $0 == "_" || $0.isLetter || $0.isNumber }
    }

    public static func extractClassInstanceSize(parent: String, systemKC: StreamingKCImage, bootKC: StreamingKCImage) -> Int? {
        let m = mangleClass(parent)
        for suffix in ["C2Ev", "C1Ev"] {
            let ctorName = "__ZN\(m)9MetaClass\(suffix)"
            let ownerKC: StreamingKCImage?
            let va: UInt64
            if let v = systemKC.symbolAddress(ctorName) {
                ownerKC = systemKC; va = v
            } else if let v = bootKC.symbolAddress(ctorName) {
                ownerKC = bootKC; va = v
            } else {
                continue
            }
            guard let kc = ownerKC, let code = kc.readBytes(atVMAddr: va, count: 96), code.count >= 16 else {
                continue
            }
            let bytes = [UInt8](code)
            var i = 0
            while i + 8 < bytes.count {
                if bytes[i] == 0x48 && bytes[i + 1] == 0x8D && bytes[i + 2] == 0x35 {
                    let rel = Int32(bitPattern: code.leUInt32(i + 3))
                    let strVA = UInt64(bitPattern: Int64(bitPattern: va) &+ Int64(i + 7) &+ Int64(rel))
                    let nxt = bytes[i + 7]
                    if (nxt == 0x48 || nxt == 0x31), let s = kc.readCString(atVMAddr: strVA), isCIdentifier(s) {
                        var p = i + 7
                        if p + 7 <= bytes.count && bytes[p] == 0x48 && (bytes[p + 1] == 0x8B || bytes[p + 1] == 0x8D) && bytes[p + 2] == 0x15 {
                            p += 7
                        } else if p + 2 <= bytes.count && bytes[p] == 0x31 && bytes[p + 1] == 0xD2 {
                            p += 2
                        }
                        if p + 5 <= bytes.count && bytes[p] == 0xB9 {
                            return Int(code.leUInt32(p + 1))
                        }
                    }
                }
                i += 1
            }
        }
        return nil
    }

    public static func canonicalLayoutID(for fp: LayoutFingerprint) throws -> String {
        struct Core: Encodable {
            let arch: String
            let classes: [FPClassEntry]
            let expectClasses: Int
            let expectSlots: Int
            let schema: String
        }
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        let core = Core(arch: fp.arch, classes: fp.classes, expectClasses: fp.expectClasses, expectSlots: fp.expectSlots, schema: fp.schema)
        let data = try enc.encode(core)
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    public static func matchVariant(
        variantsDirPayload: URL,
        bootKCPath: String,
        systemKCPath: String
    ) throws -> LayoutMatchResult {
        let indexURL = variantsDirPayload.appendingPathComponent("variants/index.json")
        let indexData = try Data(contentsOf: indexURL)
        let index = try JSONDecoder().decode(VariantsIndex.self, from: indexData)
        guard index.schema == "garnet-accel-variants-v1", !index.variants.isEmpty else {
            return LayoutMatchResult(matched: false, matchedLayoutId: nil, matchedShortId: nil, matchedVariantRelPath: nil, summary: "Invalid variants/index.json schema or empty list", observations: [])
        }

        let bootKC = try StreamingKCImage(path: bootKCPath)
        try bootKC.ensureSymtabsLoaded()
        let systemKC = try StreamingKCImage(path: systemKCPath)
        try systemKC.ensureSymtabsLoaded()

        var bestObservations: [LocalClassObservation] = []
        var failureReasons: [String] = []

        for entry in index.variants {
            let fpURL = variantsDirPayload.appendingPathComponent(entry.fingerprintRelPath)
            guard let fpData = try? Data(contentsOf: fpURL),
                  let fp = try? JSONDecoder().decode(LayoutFingerprint.self, from: fpData),
                  fp.schema == "garnet-layout-fingerprint-v1",
                  fp.layoutId == entry.layoutId,
                  fp.expectClasses == 13,
                  fp.expectSlots == 2720 else {
                failureReasons.append("Variant \(entry.shortId): invalid fingerprint JSON")
                continue
            }
            if let computed = try? canonicalLayoutID(for: fp), computed != fp.layoutId {
                failureReasons.append("Variant \(entry.shortId): layoutId hash verification failed")
                continue
            }

            let pureVA = bootKC.symbolAddress("___cxa_pure_virtual")
            var observations: [LocalClassObservation] = []
            var variantOk = true

            for clsSpec in fp.classes {
                let actualSize = extractClassInstanceSize(parent: clsSpec.parent, systemKC: systemKC, bootKC: bootKC)
                if actualSize != clsSpec.instanceSize {
                    variantOk = false
                }
                let vtName = "__ZTV\(mangleClass(clsSpec.parent))"
                guard let vtVA = systemKC.symbolAddress(vtName) else {
                    variantOk = false
                    observations.append(LocalClassObservation(
                        leaf: clsSpec.leaf, parent: clsSpec.parent,
                        expectedSize: clsSpec.instanceSize, actualSize: actualSize,
                        expectedSlots: clsSpec.nslots, actualSlots: nil,
                        mismatchedSlots: ["missing vtable \(vtName)"]
                    ))
                    continue
                }
                let endVA = systemKC.nextSymbolAddress(after: vtVA) ?? (vtVA + UInt64(clsSpec.nslots * 8 + 64))
                let maxPossible = Int((endVA > vtVA + 16 ? (endVA - vtVA - 16) / 8 : 0))
                var actualSlots = 0
                if maxPossible > 0, let rawTable = systemKC.readBytes(atVMAddr: vtVA + 16, count: maxPossible * 8) {
                    while actualSlots < maxPossible {
                        if rawTable.leUInt64(actualSlots * 8) == 0 { break }
                        actualSlots += 1
                    }
                    if actualSlots != clsSpec.nslots { variantOk = false }

                    var badSlots: [String] = []
                    for binding in clsSpec.inheritedBindings {
                        if binding.slot >= actualSlots {
                            variantOk = false
                            badSlots.append("slot \(binding.slot) out of range (\(actualSlots))")
                            continue
                        }
                        let rawSlot = rawTable.leUInt64(binding.slot * 8)
                        let (lvl, targetVA) = decodeChainedFixup(rawSlot)
                        if lvl == 0 && pureVA != nil && targetVA == pureVA {
                            variantOk = false
                            badSlots.append("slot \(binding.slot) unexpectedly pure virtual")
                            continue
                        }
                        let targetSyms = (lvl == 0 ? bootKC : systemKC).symbols(at: targetVA)
                        let expectedSet = Set(binding.syms)
                        if expectedSet.isDisjoint(with: targetSyms) {
                            variantOk = false
                            if badSlots.count < 8 {
                                let got = targetSyms.isEmpty ? [String(format: "@%#llx", targetVA)] : targetSyms
                                badSlots.append("slot \(binding.slot): want \(binding.syms.first ?? "?") got \(got.joined(separator: ","))")
                            }
                        }
                    }
                    observations.append(LocalClassObservation(
                        leaf: clsSpec.leaf, parent: clsSpec.parent,
                        expectedSize: clsSpec.instanceSize, actualSize: actualSize,
                        expectedSlots: clsSpec.nslots, actualSlots: actualSlots,
                        mismatchedSlots: badSlots
                    ))
                } else {
                    variantOk = false
                    observations.append(LocalClassObservation(
                        leaf: clsSpec.leaf, parent: clsSpec.parent,
                        expectedSize: clsSpec.instanceSize, actualSize: actualSize,
                        expectedSlots: clsSpec.nslots, actualSlots: nil,
                        mismatchedSlots: ["unreadable vtable memory"]
                    ))
                }
            }

            if bestObservations.isEmpty { bestObservations = observations }
            if variantOk {
                return LayoutMatchResult(
                    matched: true,
                    matchedLayoutId: entry.layoutId,
                    matchedShortId: entry.shortId,
                    matchedVariantRelPath: entry.accelBundleRelPath,
                    summary: "KC layout matched variant \(entry.shortId) (13 classes / 2720 slots)",
                    observations: observations
                )
            } else {
                let badCount = observations.filter { $0.actualSize != $0.expectedSize || $0.actualSlots != $0.expectedSlots || !$0.mismatchedSlots.isEmpty }.count
                failureReasons.append("Variant \(entry.shortId): \(badCount) class(es) mismatched")
            }
        }

        return LayoutMatchResult(
            matched: false,
            matchedLayoutId: nil,
            matchedShortId: nil,
            matchedVariantRelPath: nil,
            summary: failureReasons.joined(separator: "; "),
            observations: bestObservations
        )
    }
}

public extension Data {
    func leUInt32(_ offset: Int) -> UInt32 {
        withUnsafeBytes { $0.baseAddress!.assumingMemoryBound(to: UInt8.self).loadUnalignedLE32(at: offset) }
    }

    func leUInt64(_ offset: Int) -> UInt64 {
        withUnsafeBytes { $0.baseAddress!.assumingMemoryBound(to: UInt8.self).loadUnalignedLE64(at: offset) }
    }

    func fixedCString(at offset: Int, maxLen: Int) -> String {
        let limit = Swift.min(count, offset + maxLen)
        guard offset < limit else { return "" }
        var end = offset
        while end < limit && self[end] != 0 { end += 1 }
        return String(decoding: self[offset..<end], as: UTF8.self)
    }
}

private extension UnsafePointer where Pointee == UInt8 {
    func loadUnalignedLE32(at offset: Int) -> UInt32 {
        UInt32(self[offset]) |
        (UInt32(self[offset + 1]) << 8) |
        (UInt32(self[offset + 2]) << 16) |
        (UInt32(self[offset + 3]) << 24)
    }

    func loadUnalignedLE64(at offset: Int) -> UInt64 {
        UInt64(loadUnalignedLE32(at: offset)) |
        (UInt64(loadUnalignedLE32(at: offset + 4)) << 32)
    }
}
