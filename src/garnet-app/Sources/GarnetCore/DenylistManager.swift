import Foundation

public struct DenylistEntry: Codable, Equatable, Identifiable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case bundle
        case proc
    }

    public var id: String { rawLine }
    public let kind: Kind
    public let value: String

    public var rawLine: String {
        "\(kind.rawValue):\(value)"
    }
}

public struct AppBundleMetadata: Codable, Equatable, Sendable {
    public let appPath: String
    public let bundleIdentifier: String?
    public let executableName: String?
}

public enum DenylistError: Error, CustomStringConvertible {
    case protectedTarget(String)
    case invalidEntry(String)
    case fileError(String)
    case syncFailed(String)

    public var description: String {
        switch self {
        case .protectedTarget(let m): return "Protected system component cannot be denylisted: \(m)"
        case .invalidEntry(let m): return "Invalid denylist entry: \(m)"
        case .fileError(let m): return "Denylist file error: \(m)"
        case .syncFailed(let m): return "Kernel appdeny sync failed: \(m)"
        }
    }
}

public enum DenylistManager {
    public static let denylistPath = GarnetPaths.denylistPath
    public static let garnetTestPath = GarnetPaths.garnetTestPath

    public static func resolvedReadURL(env: HostEnvironment) -> URL? {
        let primary = env.url(forSystemPath: denylistPath)
        if FileManager.default.fileExists(atPath: primary.path) {
            return primary
        }
        let legacy = env.url(forSystemPath: GarnetPaths.legacyDenylistPath)
        if FileManager.default.fileExists(atPath: legacy.path) {
            return legacy
        }
        return nil
    }

    private static func resolvedGarnetTestURL(env: HostEnvironment) -> URL? {
        let primary = env.url(forSystemPath: garnetTestPath)
        if FileManager.default.isExecutableFile(atPath: primary.path) {
            return primary
        }
        let legacy = env.url(forSystemPath: GarnetPaths.legacyGarnetTestPath)
        if FileManager.default.isExecutableFile(atPath: legacy.path) {
            return legacy
        }
        return nil
    }

    public static func inspectAppBundle(at appURL: URL) throws -> AppBundleMetadata {
        let resolved = appURL.standardizedFileURL.resolvingSymlinksInPath()
        let plistURL = resolved.appendingPathComponent("Contents/Info.plist")
        let data = try Data(contentsOf: plistURL)
        let obj = try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
        guard let dict = obj as? [String: Any] else {
            throw DenylistError.invalidEntry("Cannot parse Info.plist in \(appURL.path)")
        }
        let bid = (dict["CFBundleIdentifier"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
        var execName = (dict["CFBundleExecutable"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
        if execName == nil || execName?.isEmpty == true {
            execName = resolved.deletingPathExtension().lastPathComponent
        }
        return AppBundleMetadata(appPath: resolved.path, bundleIdentifier: bid, executableName: execName)
    }

    public static func validateValue(_ raw: String) throws -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, trimmed.utf8.count <= 255 else {
            throw DenylistError.invalidEntry("Value must be 1..255 bytes")
        }
        for scalar in trimmed.unicodeScalars where scalar.value < 0x20 || scalar.value == 0x7F {
            throw DenylistError.invalidEntry("Control characters are not allowed")
        }
        if trimmed == "WindowServer" || trimmed == "com.apple.WindowServer" {
            throw DenylistError.protectedTarget(trimmed)
        }
        return trimmed
    }

    public static func load(env: HostEnvironment) throws -> [DenylistEntry] {
        guard let url = resolvedReadURL(env: env) else {
            return []
        }
        let text = try String(contentsOf: url, encoding: .utf8)
        var entries: [DenylistEntry] = []
        var seen: Set<String> = []
        for rawLine in text.components(separatedBy: .newlines) {
            let trimmed = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            if trimmed.hasPrefix("bundle:") {
                let val = String(trimmed.dropFirst("bundle:".count)).trimmingCharacters(in: .whitespaces)
                if !val.isEmpty {
                    let e = DenylistEntry(kind: .bundle, value: val)
                    if seen.insert(e.rawLine).inserted { entries.append(e) }
                }
            } else if trimmed.hasPrefix("proc:") {
                let val = String(trimmed.dropFirst("proc:".count)).trimmingCharacters(in: .whitespaces)
                if !val.isEmpty {
                    let e = DenylistEntry(kind: .proc, value: val)
                    if seen.insert(e.rawLine).inserted { entries.append(e) }
                }
            }
        }
        return entries
    }

    public static func save(_ entries: [DenylistEntry], env: HostEnvironment) throws {
        for e in entries {
            _ = try validateValue(e.value)
        }
        let url = env.url(forSystemPath: denylistPath)
        let dir = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        if FileManager.default.fileExists(atPath: url.path) {
            let vals = try url.resourceValues(forKeys: [.isSymbolicLinkKey])
            if vals.isSymbolicLink == true {
                throw DenylistError.fileError("Refusing symbolic-link denylist: \(url.path)")
            }
        }
        var lines = [
            "# Garnet Application Denylist (managed by Garnet / garnetctl)",
            "# WindowServer and GARNET_ALLOW=1 root tools remain unconditionally immune.",
            ""
        ]
        for e in entries {
            lines.append(e.rawLine)
        }
        let content = lines.joined(separator: "\n") + "\n"
        try content.write(to: url, atomically: true, encoding: .utf8)
    }

    @discardableResult
    public static func add(kind: DenylistEntry.Kind, value: String, env: HostEnvironment, syncLiveKernel: Bool = true) throws -> [DenylistEntry] {
        let clean = try validateValue(value)
        var current = try load(env: env)
        let entry = DenylistEntry(kind: kind, value: clean)
        if !current.contains(entry) {
            current.append(entry)
            try save(current, env: env)
        }
        if syncLiveKernel && kind == .proc {
            syncProcEntry(action: "add", name: clean, env: env)
        }
        return current
    }

    @discardableResult
    public static func addAppBundle(at appURL: URL, env: HostEnvironment, includeProcFallback: Bool = true, syncLiveKernel: Bool = true) throws -> (metadata: AppBundleMetadata, entries: [DenylistEntry]) {
        let meta = try inspectAppBundle(at: appURL)
        var current = try load(env: env)
        if let bid = meta.bundleIdentifier, !bid.isEmpty {
            _ = try validateValue(bid)
            let bEntry = DenylistEntry(kind: .bundle, value: bid)
            if !current.contains(bEntry) { current.append(bEntry) }
        }
        if includeProcFallback, let exec = meta.executableName, !exec.isEmpty {
            _ = try validateValue(exec)
            let pEntry = DenylistEntry(kind: .proc, value: exec)
            if !current.contains(pEntry) { current.append(pEntry) }
            if syncLiveKernel {
                syncProcEntry(action: "add", name: exec, env: env)
            }
        }
        try save(current, env: env)
        return (meta, current)
    }

    @discardableResult
    public static func remove(kind: DenylistEntry.Kind, value: String, env: HostEnvironment, syncLiveKernel: Bool = true) throws -> [DenylistEntry] {
        let clean = value.trimmingCharacters(in: .whitespacesAndNewlines)
        var current = try load(env: env)
        current.removeAll { $0.kind == kind && $0.value == clean }
        try save(current, env: env)
        if syncLiveKernel && kind == .proc && !clean.isEmpty {
            syncProcEntry(action: "remove", name: clean, env: env)
        }
        return current
    }

    public static func clear(env: HostEnvironment, syncLiveKernel: Bool = true) throws {
        try save([], env: env)
        if syncLiveKernel, let toolURL = resolvedGarnetTestURL(env: env) {
            _ = env.runner.run(toolURL.path, ["accel", "appdeny", "clear"])
        }
    }

    public static func syncAllToLiveKernel(env: HostEnvironment) {
        guard let toolURL = resolvedGarnetTestURL(env: env),
              let entries = try? load(env: env) else {
            return
        }
        _ = env.runner.run(toolURL.path, ["accel", "appdeny", "clear"])
        for e in entries where e.kind == .proc {
            _ = env.runner.run(toolURL.path, ["accel", "appdeny", "add", e.value])
        }
    }

    private static func syncProcEntry(action: String, name: String, env: HostEnvironment) {
        guard let toolURL = resolvedGarnetTestURL(env: env) else { return }
        _ = env.runner.run(toolURL.path, ["accel", "appdeny", action, name])
    }
}
