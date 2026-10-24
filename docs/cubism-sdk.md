# Cubism SDK Dependencies

The project pins the official Native and Web **5 R5 (`5-r.5`)** SDK releases. Complete packages, including Core, Framework, version information, license files, and official samples, are extracted into the ignored `.cache/cubism` directory.

## Download and configuration

From the repository root:

```sh
python3 tools/fetch_cubism.py
```

Use `--sdk native` or `--sdk web` to download only one package. The preview's Mao, Haru, and Hiyori files are stored under `examples/preview/resources/raw/` and are packaged directly on all platforms. The optional Wanko integration test still reads its model from the Native SDK samples. The script downloads the official release archives, verifies their pinned hashes, and does not submit an email address or register a subscription.

To use an external copy of the same release, set these CMake cache variables before configuring the consumer:

| Variable | Default location in this repository |
| --- | --- |
| `LIVE2D_CUBISM_NATIVE_ROOT` | `.cache/cubism/CubismSdkForNative-5-r.5` |
| `LIVE2D_CUBISM_WEB_ROOT` | `.cache/cubism/CubismSdkForWeb-5-r.5` |

The wrapper's MIT license does not cover Cubism or the sample models. Their bundled licenses apply separately. Downloaded SDKs and build caches remain excluded from the wrapper's source distribution. The preview includes three original sample model directories with [upstream notices](../examples/preview/resources/raw/model_licenses/NOTICE.txt); they retain their separate model licenses.

## Core binaries in the packages

This inventory was checked against the downloaded files and their README documents. Package availability does not establish wrapper support for every architecture.

| Platform | Package contents |
| --- | --- |
| Android | Static and shared Core for arm64-v8a, x86, and x86_64; no armeabi-v7a Core in this release |
| iOS | Static Core for arm64 devices and arm64/x86_64 Simulator, including Debug/Release |
| macOS | Static and dynamic Core for arm64 and x86_64 |
| Windows | Dynamic Core for x86/x86_64 and static MD/MDd/MT/MTd variants for VC++ 141/142/143 |
| Linux | x86_64 Core; ARM64 is under `experimental` and is not claimed as validated support |
| Web | Separate JavaScript Core, TypeScript declarations, Web Framework, and browser samples |

Native renderer sources for OpenGL, Metal, and D3D11 are in Framework. Web uses its own SDK rather than compiling Native Core into WASM. macOS Metal integration has passed host tests and manual display/input checks; the presence of a Core binary alone is not evidence of renderer compatibility.

## Pinned checksums

| Archive | SHA-256 |
| --- | --- |
| `CubismSdkForNative-5-r.5.zip` | `7ff3a4bbc19c0a8728965aa522ab77eb11b252916453e68a8a78d3b71188bb12` |
| `CubismSdkForWeb-5-r.5.zip` | `67064a7fb1812cf502f5c4a03bfe12cc638c75a621bb4acf06bb28763df06ba0` |

These hashes were recorded from the official HTTPS downloads to pin content and detect later changes. They are not vendor signatures. The download script checks existing archives as well as new downloads.

## Toolchains and compatibility

The following are local observations from 2026-09-10, not general platform guarantees:

- HuxerUI CLI/SDK 0.3.0 was used. Xcode and the Android SDK/NDK were available. Android builds used JDK 17; the local default JDK 11 did not satisfy Gradle requirements.
- The installed HuxerUI Web library directory is named `emscripten-4.0.19`. Linking with an official Emscripten 4.0.19 installation failed with a missing `std::__2::__hash_memory` symbol. Emscripten 6.0.5 built and ran the preview successfully. Check the actual SDK/toolchain pairing rather than choosing solely by directory name. No global shell configuration or HuxerUI SDK files were changed.
- `xcrun otool -l` reports deployment markers of macOS 15.7 for the arm64 Native Core and iOS 26.2 for its arm64 device and Simulator slices. The generated application still has lower deployment targets, so linking emits warnings. Those markers do not establish a tested minimum OS. Older OS compatibility and a possible earlier Core baseline remain unresolved.
- The archive release name is 5 R5; the loaded Native Core reports version 6.0.1. Treat the bundled binaries and their metadata as authoritative rather than inferring a Core version from the archive name.
- Windows still requires a build and runtime validation on a suitable host or CI. Linux x86_64 build and first-frame rendering are recorded in [validation records](../tests/README.md#linux-opengl-es); visual and input checks remain pending.

First-time Android shader preparation can be slow even though it now runs outside the UI thread. Texture upload and the first draw still have synchronous costs. See [loading validation](../tests/README.md#loading-responsiveness) for measurements and limits.

## Official sources

- [Native download page](https://www.live2d.com/en/sdk/download/native/)
- [Web download page](https://www.live2d.com/en/sdk/download/web/)
- [Native release listing](https://cubism.live2d.com/sdk-native/js/download.js)
- [Web release listing](https://cubism.live2d.com/sdk-web/js/download.js)
