// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "gpu-tray",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "gpu-tray",
            path: "Sources/GPUTray",
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
