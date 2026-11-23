// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "GarnetInstaller",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .library(name: "GarnetCore", targets: ["GarnetCore"]),
        .executable(name: "garnetctl", targets: ["GarnetCLI"]),
        .executable(name: "Garnet", targets: ["GarnetApp"]),
    ],
    targets: [
        .target(
            name: "GarnetCore",
            path: "Sources/GarnetCore"
        ),
        .executableTarget(
            name: "GarnetCLI",
            dependencies: ["GarnetCore"],
            path: "Sources/GarnetCLI"
        ),
        .executableTarget(
            name: "GarnetApp",
            dependencies: ["GarnetCore"],
            path: "Sources/GarnetApp"
        ),
        .executableTarget(
            name: "GarnetCoreTests",
            dependencies: ["GarnetCore"],
            path: "Tests/GarnetCoreTests"
        ),
    ]
)
