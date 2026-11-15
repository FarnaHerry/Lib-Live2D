// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Live2D",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "Live2D",
            targets: ["Live2D"]
        ),
    ],
    targets: [
        .target(name: "Live2D"),
    ]
)
