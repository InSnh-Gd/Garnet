import Foundation

/// Single source of truth for deployed macOS system-disk runtime paths and
/// legacy v0.1.0 migration/cleanup locations.
///
/// NOTE: `/Library/Application Support/RDNA4/hardware-test` (and its `backups/`)
/// belongs to the physical hardware-test harness and MUST NEVER be modified or
/// removed by installer/uninstaller cleanup.
public enum GarnetPaths {
    public static let auxKCPath = "/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
    public static let extensionsDir = "/Library/Extensions"
    public static let gpuBundlesDir = "/Library/GPUBundles"
    public static let launchDaemonsDir = "/Library/LaunchDaemons"

    public static let supportRoot = "/Library/Application Support/Garnet"
    public static let nativeSupportDir = "/Library/Application Support/Garnet/native"
    public static let backupsDir = "/Library/Application Support/Garnet/backups"
    public static let denylistPath = "/Library/Application Support/Garnet/native/app-denylist.txt"
    public static let profilePath = "/Library/Application Support/Garnet/native/profile"
    public static let garnetTestPath = "/Library/Application Support/Garnet/native/garnettest"
    public static let manifestPath = "/Library/Application Support/Garnet/native/MANIFEST.txt"
    public static let recoveryDaemonPath = "/Library/LaunchDaemons/com.garnet.recovery.plist"
    public static let recoveryScriptPath = "/Library/Application Support/Garnet/native/recover-uninstall.sh"
    public static let selfContainedCLIPath = "/Library/Application Support/Garnet/native/garnetctl"

    public static let logsDir = "/Library/Logs/Garnet"
    public static let recoveryLogPath = "/Library/Logs/Garnet/recovery.log"
    public static let recoveryCompletePath = "/Library/Logs/Garnet/recovery-complete.txt"
    public static let translateLogsDir = "/Library/Logs/GarnetTranslate"
    public static let xlateDumpDir = "/private/tmp/garnet-xlate"
    public static let spvCacheDir = "/private/var/tmp/garnet-spv"

    public static let vulkanICDDir = "/Library/Vulkan/icd.d"
    public static let vulkanICDPath = "/Library/Vulkan/icd.d/radeon_icd.x86_64.json"
    public static let radvAppBinPath = "/usr/local/bin/radv-app"

    // macOS icon-services & Dock icon cache locations cleared best-effort on install/uninstall.
    public static let systemIconCacheStorePath = "/Library/Caches/com.apple.iconservices.store"
    public static let varFoldersDir = "/private/var/folders"

    // Legacy v0.1.0 runtime paths (migrated/cleaned on upgrade; never includes RDNA4/hardware-test).
    public static let legacyNativeSupportDir = "/Library/Application Support/RDNA4/native"
    public static let legacyDenylistPath = "/Library/Application Support/RDNA4/native/app-denylist.txt"
    public static let legacyProfilePath = "/Library/Application Support/RDNA4/native/profile"
    public static let legacyGarnetTestPath = "/Library/Application Support/RDNA4/native/garnettest"
    public static let legacyManifestPath = "/Library/Application Support/RDNA4/native/MANIFEST.txt"
    public static let legacyLogsDir = "/Library/Logs/RDNA4"
    public static let legacyTranslateLogsDir = "/Library/Logs/RDNA4Translate"
    public static let legacyXlateDumpDir = "/private/tmp/rdna4m"
    public static let legacySpvCacheDir = "/private/var/tmp/rdna4m-spv"

    public static let legacyRuntimeCleanupPaths: [String] = [
        legacyNativeSupportDir,
        legacyXlateDumpDir,
        legacySpvCacheDir
    ]
}
