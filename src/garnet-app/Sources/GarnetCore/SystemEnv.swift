import Foundation

public struct CommandResult: Sendable {
    public let exitCode: Int32
    public let stdout: String
    public let stderr: String

    public init(exitCode: Int32, stdout: String, stderr: String) {
        self.exitCode = exitCode
        self.stdout = stdout
        self.stderr = stderr
    }
}

public protocol CommandRunning: Sendable {
    func run(_ executable: String, arguments: [String], environment: [String: String]?) -> CommandResult
}

public extension CommandRunning {
    func run(_ executable: String, _ arguments: [String]) -> CommandResult {
        run(executable, arguments: arguments, environment: nil)
    }
}

public final class ProcessCommandRunner: CommandRunning, @unchecked Sendable {
    public init() {}

    public func run(_ executable: String, arguments: [String], environment: [String: String]? = nil) -> CommandResult {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: executable)
        proc.arguments = arguments
        if let environment {
            var merged = ProcessInfo.processInfo.environment
            for (k, v) in environment { merged[k] = v }
            proc.environment = merged
        }
        let outPipe = Pipe()
        let errPipe = Pipe()
        proc.standardOutput = outPipe
        proc.standardError = errPipe
        do {
            try proc.run()
        } catch {
            return CommandResult(exitCode: 127, stdout: "", stderr: error.localizedDescription)
        }
        // Drain stdout/stderr before waitUntilExit to avoid pipe buffer deadlocks.
        let outData = outPipe.fileHandleForReading.readDataToEndOfFile()
        let errData = errPipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        return CommandResult(
            exitCode: proc.terminationStatus,
            stdout: String(decoding: outData, as: UTF8.self),
            stderr: String(decoding: errData, as: UTF8.self)
        )
    }
}

public struct PCIGPUInfo: Codable, Equatable, Sendable {
    public let vendorID: UInt32
    public let deviceID: UInt32
    public let classCode: UInt32
    public let model: String
    public let hasAssignedAddresses: Bool
    public let maxAssignedBarSizeBytes: UInt64

    public var pciIDString: String {
        String(format: "%04x:%04x", vendorID & 0xFFFF, deviceID & 0xFFFF)
    }

    public var isNavi44: Bool {
        vendorID == 0x1002 && deviceID == 0x7590
    }

    public var isNavi48: Bool {
        vendorID == 0x1002 && (deviceID == 0x7550 || deviceID == 0x7551)
    }
}

public struct SIPStatusInfo: Codable, Equatable, Sendable {
    public let csrActiveConfigHex: String?
    public let csrValue: UInt32?
    public let kextSigningDisabled: Bool
    public let fsProtectionsDisabled: Bool
    public let readyForAuxKC: Bool
    public let bootArgs: String?
    public let detail: String
}

public struct HostEnvironment: Sendable {
    public let sysRoot: URL
    public let bootKCPath: String
    public let systemKCPath: String
    public let runner: CommandRunning
    public let effectiveUID: UInt32

    public init(
        sysRoot: URL = URL(fileURLWithPath: "/"),
        bootKCPath: String = "/System/Library/KernelCollections/BootKernelExtensions.kc",
        systemKCPath: String = "/System/Library/KernelCollections/SystemKernelExtensions.kc",
        runner: CommandRunning = ProcessCommandRunner(),
        effectiveUID: UInt32 = geteuid()
    ) {
        self.sysRoot = sysRoot
        self.bootKCPath = bootKCPath
        self.systemKCPath = systemKCPath
        self.runner = runner
        self.effectiveUID = effectiveUID
    }

    public func url(forSystemPath absPath: String) -> URL {
        if sysRoot.path == "/" {
            return URL(fileURLWithPath: absPath)
        }
        let trimmed = absPath.hasPrefix("/") ? String(absPath.dropFirst()) : absPath
        return sysRoot.appendingPathComponent(trimmed)
    }

    public func readProductVersion() -> String {
        runner.run("/usr/bin/sw_vers", ["-productVersion"]).stdout.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public func readBuildVersion() -> String {
        runner.run("/usr/bin/sw_vers", ["-buildVersion"]).stdout.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public func readArch() -> String {
        runner.run("/usr/bin/uname", ["-m"]).stdout.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public func readIsVM() -> Bool {
        let out = runner.run("/usr/sbin/sysctl", ["-n", "machdep.cpu.features"]).stdout
        return out.split(whereSeparator: { $0.isWhitespace }).contains("VMM")
    }

    public static func isMacOS26Version(_ version: String) -> Bool {
        version == "26" || version.hasPrefix("26.")
    }

    public func readSIPStatus() -> SIPStatusInfo {
        let ioregOut = runner.run("/usr/sbin/ioreg", ["-rd1", "-p", "IODeviceTree", "-n", "options"]).stdout
        let csrutilOut = runner.run("/usr/bin/csrutil", ["status"]).stdout

        var csrVal: UInt32?
        var csrHex: String?
        var bootArgs: String?

        for rawLine in ioregOut.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.contains("\"csr-active-config\""), let lt = line.firstIndex(of: "<"), let gt = line.firstIndex(of: ">"), lt < gt {
                let rawHex = String(line[line.index(after: lt)..<gt]).replacingOccurrences(of: " ", with: "")
                if rawHex.count == 8, let be = UInt32(rawHex, radix: 16) {
                    let le = be.byteSwapped
                    csrVal = le
                    csrHex = String(format: "0x%04x", le)
                }
            } else if line.contains("\"boot-args\""), let eq = line.range(of: "=") {
                var rhs = String(line[eq.upperBound...]).trimmingCharacters(in: .whitespaces)
                if rhs.hasPrefix("\"") && rhs.hasSuffix("\"") && rhs.count >= 2 {
                    rhs = String(rhs.dropFirst().dropLast())
                }
                bootArgs = rhs
            }
        }

        let kextOffByUtil = csrutilOut.contains("Kext Signing: disabled")
        let fsOffByUtil = csrutilOut.contains("Filesystem Protections: disabled")
        let csrBitsOk = (csrVal ?? 0) & 0x003 == 0x003
        let kextOff = kextOffByUtil || csrBitsOk
        let fsOff = fsOffByUtil || csrBitsOk
        let ready = kextOff && fsOff
        let detail: String
        if ready {
            detail = "Kext Signing & Filesystem Protections disabled (csr-active-config \(csrHex ?? "custom"))"
        } else {
            detail = "SIP blocks /Library/Extensions & AuxKC rebuild (need OpenCore csr-active-config = 03080000 and SecureBootModel = Disabled)"
        }
        return SIPStatusInfo(
            csrActiveConfigHex: csrHex,
            csrValue: csrVal,
            kextSigningDisabled: kextOff,
            fsProtectionsDisabled: fsOff,
            readyForAuxKC: ready,
            bootArgs: bootArgs,
            detail: detail
        )
    }

    public func readPCIGPUs() -> [PCIGPUInfo] {
        let out = runner.run("/usr/sbin/ioreg", ["-r", "-c", "IOPCIDevice", "-d", "1", "-l"]).stdout
        var gpus: [PCIGPUInfo] = []

        var curVendor: UInt32?
        var curDevice: UInt32?
        var curClass: UInt32?
        var curModel: String = ""
        var curMaxBar: UInt64 = 0
        var curHasAssigned = false

        func flushCurrent() {
            if let v = curVendor, let d = curDevice, let c = curClass {
                // PCI display controller base class is 0x03 (little-endian raw dword: byte 2 == 0x03 -> 0x030000..0x03ffff)
                if (c >> 16) == 0x03 {
                    gpus.append(PCIGPUInfo(
                        vendorID: v,
                        deviceID: d,
                        classCode: c,
                        model: curModel.isEmpty ? "Unknown GPU" : curModel,
                        hasAssignedAddresses: curHasAssigned,
                        maxAssignedBarSizeBytes: curMaxBar
                    ))
                }
            }
            curVendor = nil
            curDevice = nil
            curClass = nil
            curModel = ""
            curMaxBar = 0
            curHasAssigned = false
        }

        for rawLine in out.components(separatedBy: .newlines) {
            let trimmed = rawLine.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("+-o ") {
                flushCurrent()
                continue
            }
            if trimmed.contains("\"vendor-id\""), let val = parseLEDWord(from: trimmed) {
                curVendor = val
            } else if trimmed.contains("\"device-id\""), let val = parseLEDWord(from: trimmed) {
                curDevice = val
            } else if trimmed.contains("\"class-code\""), let val = parseLEDWord(from: trimmed) {
                curClass = val
            } else if trimmed.contains("\"model\""), let eq = trimmed.range(of: "=") {
                var rhs = String(trimmed[eq.upperBound...]).trimmingCharacters(in: .whitespaces)
                if rhs.hasPrefix("\"") && rhs.hasSuffix("\"") && rhs.count >= 2 {
                    rhs = String(rhs.dropFirst().dropLast())
                } else if let lt = rhs.firstIndex(of: "<"), let gt = rhs.firstIndex(of: ">"), lt < gt {
                    rhs = String(rhs[rhs.index(after: lt)..<gt])
                }
                curModel = rhs
            } else if trimmed.contains("\"assigned-addresses\""), let lt = trimmed.firstIndex(of: "<"), let gt = trimmed.firstIndex(of: ">"), lt < gt {
                curHasAssigned = true
                let hexStr = String(trimmed[trimmed.index(after: lt)..<gt]).replacingOccurrences(of: " ", with: "")
                // Each assigned-addresses tuple is 5 little-endian uint32 words (20 bytes = 40 hex chars); words 3,4 form 64-bit size (hi, lo).
                var idx = hexStr.startIndex
                while hexStr.distance(from: idx, to: hexStr.endIndex) >= 40 {
                    let s3Start = hexStr.index(idx, offsetBy: 24)
                    let s3End = hexStr.index(idx, offsetBy: 32)
                    let s4End = hexStr.index(idx, offsetBy: 40)
                    if let hiLE = UInt32(hexStr[s3Start..<s3End], radix: 16)?.byteSwapped,
                       let loLE = UInt32(hexStr[s3End..<s4End], radix: 16)?.byteSwapped {
                        let sz = (UInt64(hiLE) << 32) | UInt64(loLE)
                        if sz > curMaxBar { curMaxBar = sz }
                    }
                    idx = s4End
                }
            }
        }
        flushCurrent()
        return gpus
    }

    private func parseLEDWord(from line: String) -> UInt32? {
        guard let lt = line.firstIndex(of: "<"), let gt = line.firstIndex(of: ">"), lt < gt else {
            return nil
        }
        let hex = String(line[line.index(after: lt)..<gt]).replacingOccurrences(of: " ", with: "")
        guard hex.count == 8, let be = UInt32(hex, radix: 16) else { return nil }
        return be.byteSwapped
    }
}
