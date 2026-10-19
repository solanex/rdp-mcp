// swift-tools-version: 6.0
// Mac-app "RDP MCP": adresboek, sleutelhanger en tabs rond de rdpmcp-kern.
// Bouwen: mac/build-app.sh (verwacht FreeRDP 3 via Homebrew).
import PackageDescription

let brew = "/opt/homebrew"

let package = Package(
    name: "RDPMCP",
    platforms: [.macOS("26.0")],
    targets: [
        .target(
            name: "CRdpMcpCore",
            path: "core",
            sources: ["src"],
            publicHeadersPath: "include",
            cSettings: [
                .unsafeFlags([
                    "-I\(brew)/include/freerdp3", "-I\(brew)/include/winpr3", "-I\(brew)/include",
                    "-Wno-unused-parameter",
                ]),
            ],
            linkerSettings: [
                .unsafeFlags(["-L\(brew)/lib"]),
                .linkedLibrary("freerdp3"),
                .linkedLibrary("freerdp-client3"),
                .linkedLibrary("winpr3"),
            ]
        ),
        .executableTarget(
            name: "RDPMCP",
            dependencies: ["CRdpMcpCore"],
            path: "mac/Sources/RDPMCP",
            swiftSettings: [.swiftLanguageMode(.v5)],
            linkerSettings: [.linkedFramework("LocalAuthentication"), .linkedFramework("Carbon")]
        ),
    ]
)
