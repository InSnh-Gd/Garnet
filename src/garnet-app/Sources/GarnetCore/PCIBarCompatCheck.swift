import Foundation

public enum PCIBarCompatMode: String, Codable, Equatable, Sendable {
    case none
    case exactUUID = "exact-uuid"
    case structural
}

public struct PCIBarCompatResult: Codable, Equatable, Sendable {
    public let compatible: Bool
    public let mode: PCIBarCompatMode
    public let status: String
    public let iopciFamilyUUID: String?
    public let entryHdrOffsetHex: String?

    public init(compatible: Bool, mode: PCIBarCompatMode, status: String, iopciFamilyUUID: String?, entryHdrOffsetHex: String?) {
        self.compatible = compatible
        self.mode = mode
        self.status = status
        self.iopciFamilyUUID = iopciFamilyUUID
        self.entryHdrOffsetHex = entryHdrOffsetHex
    }
}

public enum PCIBarCompatCheck {
    // Verified 25G332 IOPCIFamily UUID and signatures matching src/garnet-pci-compat/src/pci_bar_compat_pure.h
    public static let expected25G332UUID: [UInt8] = [
        0x22, 0x77, 0x71, 0xC6, 0xAD, 0xC2, 0x30, 0xC7,
        0x84, 0x08, 0x26, 0x3F, 0x72, 0x3C, 0xF8, 0x1C
    ]

    private static let prologueSig: [UInt8] = [
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
        0x58, 0x89, 0x4D, 0xB4, 0x41, 0x89, 0xD6, 0x49,
        0x89, 0xF5, 0x48, 0x89
    ]
    private static let site1Sig: [UInt8] = [
        0x48, 0x3D, 0x01, 0x00, 0x00, 0x40, 0x44, 0x8B,
        0x75, 0xD4, 0x41, 0x83, 0xD6, 0xFF
    ]
    private static let site2Sig: [UInt8] = [
        0x48, 0x3D, 0x01, 0x00, 0x00, 0x40, 0x89, 0xC1,
        0x48, 0x0F, 0x42, 0xC8, 0x0F, 0x92, 0xC0, 0x20,
        0xC2
    ]

    private static let ctxRead16: [UInt8] = [0xBA, 0x04, 0x00, 0x00, 0x00, 0x31, 0xC9]
    private static let ctxWrite16: [UInt8] = [0xBA, 0x04, 0x00, 0x00, 0x00, 0x45, 0x31, 0xC0]
    private static let ctxRead32: [UInt8] = [0x4C, 0x89, 0xEE, 0x44, 0x89, 0xFA, 0x31, 0xC9]
    private static let ctxWrite32: [UInt8] = [0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0x45, 0x31, 0xC0]
    private static let anchorClassHigh2E: [UInt8] = [0x41, 0x0F, 0xB7, 0x45, 0x2E, 0xC1, 0xE0, 0x10, 0x3D, 0x00, 0x00, 0x03, 0x00]
    private static let anchorBusNum32: [UInt8] = [0x48, 0x8B, 0x45, 0xC8, 0x80, 0x78, 0x32, 0x00]
    private static let anchorRangesInitCtx: [UInt8] = [
        0x48, 0x8B, 0x55, 0xC8, 0x4C, 0x8B, 0x74, 0xC2, 0x60, 0x4C, 0x89, 0xF7,
        0x44, 0x89, 0xEE, 0x4C, 0x89, 0xFA, 0x49, 0x89, 0xC8
    ]
    private static let anchorFinalizeRange: [UInt8] = [
        0x49, 0x89, 0x46, 0x40, 0x41, 0x80, 0xFC, 0x01, 0x75, 0x1A, 0x41, 0x80,
        0x4E, 0x54, 0x80, 0x48, 0x8B, 0x45, 0xC0, 0xF6, 0x80, 0x91, 0x00, 0x00,
        0x00, 0x20, 0x74, 0x08, 0x49, 0xC7, 0x46, 0x48, 0xFF, 0xFF, 0xFF, 0xFF
    ]
    private static let helperPrologue4: [UInt8] = [0x55, 0x48, 0x89, 0xE5]

    private static let minProbeFuncLen = 0x41A
    private static let site1RelOffset = 0x2B2
    private static let site2RelOffset = 0x3B7
    private static let expectedEntryHdrOff: UInt64 = 0x3712E
    private static let callSites: [(site: Int, expectedHdr: UInt64)] = [
        (0x24, 0x306D8),
        (0x46, 0x32AE6),
        (0x124, 0x2F9E0),
        (0x13C, 0x32B6A),
        (0x3E3, 0x3EEBF),
    ]

    public static func formatUUID(_ bytes: [UInt8]) -> String {
        guard bytes.count == 16 else { return "00000000-0000-0000-0000-000000000000" }
        let hex = bytes.map { String(format: "%02X", $0) }.joined()
        return "\(hex.prefix(8))-\(hex.dropFirst(8).prefix(4))-\(hex.dropFirst(12).prefix(4))-\(hex.dropFirst(16).prefix(4))-\(hex.dropFirst(20).prefix(12))"
    }

    public static func checkBootKC(path: String) -> PCIBarCompatResult {
        do {
            let kc = try StreamingKCImage(path: path)
            guard let iopci = kc.filesetEntry(identifier: "com.apple.iokit.IOPCIFamily") else {
                return PCIBarCompatResult(compatible: false, mode: .none, status: "header-not-found", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
            }
            let header = try kc.readAtFileOffset(iopci.fileoff, count: 32)
            let ncmds = header.leUInt32(16)
            let szcmds = Int(header.leUInt32(20))
            guard header.leUInt32(0) == 0xFEEDFACF, header.leUInt32(4) == 0x01000007, header.leUInt32(12) == 11,
                  ncmds > 0, ncmds <= 256, szcmds >= 8, szcmds <= 65536 else {
                return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
            }
            let cmds = try kc.readAtFileOffset(iopci.fileoff + 32, count: szcmds)
            var uuid: [UInt8]?
            var textHdrRel: UInt64?
            var textSectionSize: UInt64 = 0
            var textFileOff: UInt64 = 0
            var textVMAddr: UInt64 = 0

            var off = 0
            for _ in 0..<ncmds {
                guard off + 8 <= cmds.count else { break }
                let cmd = cmds.leUInt32(off)
                let cmdsize = Int(cmds.leUInt32(off + 4))
                guard cmdsize >= 8, off + cmdsize <= cmds.count else {
                    return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
                }
                if cmd == 0x1B && cmdsize >= 24 {
                    if uuid != nil { return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil) }
                    uuid = Array(cmds[(off + 8)..<(off + 24)])
                } else if cmd == 0x19 && cmdsize >= 72 {
                    let segName = cmds.fixedCString(at: off + 8, maxLen: 16)
                    if segName == "__TEXT" {
                        let segVM = cmds.leUInt64(off + 24)
                        let nsects = Int(cmds.leUInt32(off + 64))
                        guard 72 + nsects * 80 <= cmdsize else {
                            return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
                        }
                        for s in 0..<nsects {
                            let sOff = off + 72 + s * 80
                            let sectName = cmds.fixedCString(at: sOff, maxLen: 16)
                            if sectName == "__text" {
                                textVMAddr = cmds.leUInt64(sOff + 32)
                                textSectionSize = cmds.leUInt64(sOff + 40)
                                textFileOff = UInt64(cmds.leUInt32(sOff + 48))
                                guard textVMAddr >= segVM else {
                                    return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
                                }
                                textHdrRel = textVMAddr - segVM
                            }
                        }
                    }
                }
                off += cmdsize
            }

            guard let foundUUID = uuid, let hdrRel = textHdrRel, textSectionSize > 0, textSectionSize <= (16 << 20) else {
                return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
            }
            let textData: Data
            if let mappedOff = kc.fileOffset(forVMAddr: textVMAddr) {
                textData = try kc.readAtFileOffset(mappedOff, count: Int(textSectionSize))
            } else {
                textData = try kc.readAtFileOffset(textFileOff, count: Int(textSectionSize))
            }
            return inspectIOPCIFamilyText(uuid: foundUUID, textHdrRel: hdrRel, textBytes: textData)
        } catch {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "header-not-found", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
        }
    }

    public static func inspectMachOBytes(_ data: Data) -> PCIBarCompatResult {
        guard data.count >= 32, data.leUInt32(0) == 0xFEEDFACF, data.leUInt32(4) == 0x01000007, data.leUInt32(12) == 11 else {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
        }
        let ncmds = data.leUInt32(16)
        let szcmds = Int(data.leUInt32(20))
        guard szcmds >= 8, 32 + szcmds <= data.count else {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
        }
        var off = 32
        var uuid: [UInt8]?
        var textHdrRel: UInt64?
        var textSize: UInt64 = 0
        for _ in 0..<ncmds {
            guard off + 8 <= 32 + szcmds else { break }
            let cmd = data.leUInt32(off)
            let cmdsize = Int(data.leUInt32(off + 4))
            guard cmdsize >= 8, off + cmdsize <= 32 + szcmds else {
                return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
            }
            if cmd == 0x1B && cmdsize >= 24 {
                if uuid != nil { return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil) }
                uuid = Array(data[(off + 8)..<(off + 24)])
            } else if cmd == 0x19 && cmdsize >= 72, data.fixedCString(at: off + 8, maxLen: 16) == "__TEXT" {
                let segVM = data.leUInt64(off + 24)
                let nsects = Int(data.leUInt32(off + 64))
                for s in 0..<nsects where off + 72 + (s + 1) * 80 <= off + cmdsize {
                    let sOff = off + 72 + s * 80
                    if data.fixedCString(at: sOff, maxLen: 16) == "__text" {
                        let sVM = data.leUInt64(sOff + 32)
                        textSize = data.leUInt64(sOff + 40)
                        if sVM >= segVM { textHdrRel = sVM - segVM }
                    }
                }
            }
            off += cmdsize
        }
        guard let foundUUID = uuid, let rel = textHdrRel, Int(rel + textSize) <= data.count else {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
        }
        return inspectIOPCIFamilyText(uuid: foundUUID, textHdrRel: rel, textBytes: data.subdata(in: Int(rel)..<Int(rel + textSize)))
    }

    public static func inspectIOPCIFamilyText(uuid: [UInt8], textHdrRel: UInt64, textBytes: Data) -> PCIBarCompatResult {
        guard uuid.count == 16 else {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "bad-macho", iopciFamilyUUID: nil, entryHdrOffsetHex: nil)
        }
        let uuidStr = formatUUID(uuid)
        if uuid.allSatisfy({ $0 == 0 }) {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "uuid-mismatch", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: nil)
        }
        let bytes = [UInt8](textBytes)
        guard let (nPrologue, entryTextOff) = countMatches(in: bytes, pattern: prologueSig), nPrologue == 1 else {
            let st = (countMatches(in: bytes, pattern: prologueSig)?.0 ?? 0) > 1 ? "pattern-duplicate" : "pattern-missing"
            return PCIBarCompatResult(compatible: false, mode: .none, status: st, iopciFamilyUUID: uuidStr, entryHdrOffsetHex: nil)
        }
        guard let (nSite1, site1Off) = countMatches(in: bytes, pattern: site1Sig), nSite1 == 1 else {
            let st = (countMatches(in: bytes, pattern: site1Sig)?.0 ?? 0) > 1 ? "pattern-duplicate" : "pattern-missing"
            return PCIBarCompatResult(compatible: false, mode: .none, status: st, iopciFamilyUUID: uuidStr, entryHdrOffsetHex: nil)
        }
        guard let (nSite2, site2Off) = countMatches(in: bytes, pattern: site2Sig), nSite2 == 1 else {
            let st = (countMatches(in: bytes, pattern: site2Sig)?.0 ?? 0) > 1 ? "pattern-duplicate" : "pattern-missing"
            return PCIBarCompatResult(compatible: false, mode: .none, status: st, iopciFamilyUUID: uuidStr, entryHdrOffsetHex: nil)
        }
        guard bytes.count >= minProbeFuncLen, entryTextOff <= bytes.count - minProbeFuncLen,
              site1Off == entryTextOff + site1RelOffset, site2Off == entryTextOff + site2RelOffset else {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "pattern-offset-mismatch", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: nil)
        }

        let entryHdrOff = textHdrRel + UInt64(entryTextOff)
        let entryHex = String(format: "%#llx", entryHdrOff)
        let textLen = UInt64(bytes.count)

        if uuid == expected25G332UUID {
            if entryHdrOff != expectedEntryHdrOff {
                return PCIBarCompatResult(compatible: false, mode: .none, status: "pattern-offset-mismatch", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: entryHex)
            }
            for spec in callSites {
                guard let dstHdr = decodeE8CallHdrOff(bytes: bytes, entryTextOff: entryTextOff, site: spec.site, textHdrRel: textHdrRel),
                      dstHdr >= textHdrRel && dstHdr < textHdrRel + textLen && dstHdr == spec.expectedHdr else {
                    return PCIBarCompatResult(compatible: false, mode: .none, status: "helper-call-mismatch", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: entryHex)
                }
            }
            return PCIBarCompatResult(compatible: true, mode: .exactUUID, status: "installed", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: entryHex)
        }

        let structOk =
            memEq(bytes, entryTextOff + 0x1D, ctxRead16) &&
            memEq(bytes, entryTextOff + 0x3E, ctxWrite16) &&
            memEq(bytes, entryTextOff + 0xBF, anchorClassHigh2E) &&
            memEq(bytes, entryTextOff + 0x11C, ctxRead32) &&
            memEq(bytes, entryTextOff + 0x134, ctxWrite32) &&
            memEq(bytes, entryTextOff + 0x197, anchorBusNum32) &&
            memEq(bytes, entryTextOff + 0x3CE, anchorRangesInitCtx) &&
            memEq(bytes, entryTextOff + 0x3F6, anchorFinalizeRange)
        guard structOk else {
            return PCIBarCompatResult(compatible: false, mode: .none, status: "struct-layout-mismatch", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: entryHex)
        }

        var targets: [UInt64] = []
        for spec in callSites {
            guard let dstHdr = decodeE8CallHdrOff(bytes: bytes, entryTextOff: entryTextOff, site: spec.site, textHdrRel: textHdrRel),
                  dstHdr >= textHdrRel,
                  dstHdr <= textHdrRel + textLen - UInt64(helperPrologue4.count),
                  !(dstHdr >= entryHdrOff && dstHdr < entryHdrOff + UInt64(minProbeFuncLen)),
                  memEq(bytes, Int(dstHdr - textHdrRel), helperPrologue4),
                  !targets.contains(dstHdr) else {
                return PCIBarCompatResult(compatible: false, mode: .none, status: "helper-call-mismatch", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: entryHex)
            }
            targets.append(dstHdr)
        }

        return PCIBarCompatResult(compatible: true, mode: .structural, status: "installed", iopciFamilyUUID: uuidStr, entryHdrOffsetHex: entryHex)
    }

    private static func countMatches(in bytes: [UInt8], pattern: [UInt8]) -> (Int, Int)? {
        guard !pattern.isEmpty, bytes.count >= pattern.count else { return (0, 0) }
        var count = 0
        var first = 0
        for i in 0...(bytes.count - pattern.count) {
            if bytes[i] == pattern[0] && memEq(bytes, i, pattern) {
                if count == 0 { first = i }
                count += 1
            }
        }
        return (count, first)
    }

    private static func memEq(_ bytes: [UInt8], _ offset: Int, _ pat: [UInt8]) -> Bool {
        guard offset >= 0, offset + pat.count <= bytes.count else { return false }
        for j in 0..<pat.count where bytes[offset + j] != pat[j] { return false }
        return true
    }

    private static func decodeE8CallHdrOff(bytes: [UInt8], entryTextOff: Int, site: Int, textHdrRel: UInt64) -> UInt64? {
        let pos = entryTextOff + site
        guard pos + 5 <= bytes.count, bytes[pos] == 0xE8 else { return nil }
        let rawRel = UInt32(bytes[pos + 1]) | (UInt32(bytes[pos + 2]) << 8) | (UInt32(bytes[pos + 3]) << 16) | (UInt32(bytes[pos + 4]) << 24)
        let signed = Int64(Int32(bitPattern: rawRel))
        let dstSigned = Int64(textHdrRel) + Int64(pos) + 5 + signed
        guard dstSigned >= 0 else { return nil }
        return UInt64(dstSigned)
    }
}
