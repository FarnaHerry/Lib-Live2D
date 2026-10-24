# Live2D Validation Records

These records distinguish build success, host-contract tests, actual model integration, and visual checks. Unless stated otherwise, observations were made on 2026-09-10 with HuxerUI 0.3.0. They do not establish full cross-platform acceptance.

## Host contract tests

`host_contract.cpp` checks mounting, state notifications, and texture connection using a real HuxerUI Runtime, the composable generator, and a macOS `MetalTexture` identity. It does not load Cubism or publish GPU pixels. The separate `model_integration.cpp` tests the public wrapper with actual Core/Metal rendering.

Run from the repository root with `HUXERUI_HOME` pointing to an installed SDK that includes the testing component:

```sh
cmake -S tests -B .huxerui/tests -G Ninja -DCMAKE_PREFIX_PATH="$HUXERUI_HOME" -DCMAKE_BUILD_TYPE=Debug
cmake --build .huxerui/tests
ctest --test-dir .huxerui/tests --output-on-failure
```

The tests use a macOS-specific texture type. They are separate from the distributed library and do not add a testing dependency to consumers.

`live2d_host_contract` passed on macOS arm64 and verified:

- Texture delivery queued through the owning task scope after the first frame, followed by recomposition into a normal `Image`.
- A stable `Stack` host mounting its extension only once when replacing a placeholder with the image.
- Control notifications causing local recomposition and one subsequent unit of host work without recomposing the caller.
- No repeated recomposition or host work on unchanged later test frames.
- External `Frame` changes reaching host geometry while preserving instance and texture identity.
- External `.On` handlers receiving events through the emitter obtained from `UseEvents()`.
- Normal unmount and fixture destruction immediately after the first frame releasing bindings, without leaked textures or events to dead hosts from late tasks.

Texture connection is scheduled from `OnFrame`. Posting from the extension constructor was rejected by the installed SDK. The internal stack stretches its image to the content area. Tests place the framed component in a normal parent layout rather than relying on the window root's tight constraints.

`UiTest::Pump()` forces frame submission. These results establish state propagation and mounting behavior, not autonomous wakeup from a real platform's idle state. Command and resize wakeups still need publication-count measurements. The probe's readiness and controller are test concepts, not the public model's first-frame event or actual `ModelController`.

## Native integration tests

```sh
cmake -S examples/preview -B .huxerui/integration -G Ninja \
  -DHUXERUI_HOME="$HUXERUI_HOME" -DCMAKE_BUILD_TYPE=Debug -DLIVE2D_BUILD_INTEGRATION_TEST=ON
cmake --build .huxerui/integration --parallel 8
.huxerui/integration/live2d_integration.app/Contents/MacOS/live2d_integration
```

The macOS arm64 test passed with the official Wanko package. It covers loading, package traversal rejection, preservation of the underlying missing-file `IoError`, byte limits, Core parameter metadata, completed first-frame Metal publication and texture connection, unknown motions, priority rejection, interruption/stop events, playback IDs, controller detachment, and normal process exit.

Later additions cover worker texture-validation errors, unmount during preparation, and remounting the same asset. The test uses a real-time deadline and yields time to workers; advancing virtual frame time cannot replace waiting for background work. `ModelView` is placed in a `Column` to catch missing initial placeholder dimensions.

This test publishes real Metal frames but does not inspect the final composited image. It cannot prove orientation, alpha edges, or all model blend modes.

## Web

Run from the repository root after downloading the SDKs:

```sh
npm install --prefix .cache/web-tools --no-audit --no-fund esbuild@0.25.12 typescript@5.9.3
node .cache/web-tools/node_modules/typescript/bin/tsc -p platform/web/tsconfig.json
node tools/build_web.mjs .cache/cubism/CubismSdkForWeb-5-r.5 .cache/web-tools/bridge.js
node tests/web_core_contract.mjs
```

These commands passed. The Core test loads the real Web Core and model but substitutes image decoding. It checks the manifest, metadata, and cleanup; it is not a browser rendering test.

The preview was also built with Emscripten 6.0.5 and checked in the in-app browser. Wanko was visible, pause/resume button state changed, and a body tap produced the expected hit event. The first frame waits for asynchronous shader preparation. WebGL shaders and premultiplied-alpha upload are integrated. Paused frame counts, transparency error, and complex model behavior have not been quantified.

See [SDK compatibility](../docs/cubism-sdk.md#toolchains-and-compatibility) for the observed Emscripten 4.0.19 linker mismatch.

## Android

The Debug APK was installed on a Huawei TAS-AN00 running Android 12, arm64. Wanko displayed with a ready status. After pausing, model-region screenshots taken one second apart were identical. After resuming, the same region changed. Tapping the body produced the expected hit event. No crash or EGL texture-handoff error was reproduced in that verification run.

The fixes included embedded official OpenGL shaders with lifetime-stable Cubism load/release callbacks, model-texture mipmap generation, and fresh Android target storage on each frame. Reallocating storage detaches the previous EGLImage before the next handoff and avoids a second-publication `EGL_BAD_ACCESS`. Linux received shader/mipmap integration too, but has no runtime verification. Temporary pixel readback and diagnostic logging were removed.

The initial run did not optimize first-time shader compilation or per-frame storage-allocation cost. Background preparation was added later, as recorded below. Long-running memory pressure and complex blend models remain outside the verified scope.

## Linux OpenGL ES

On 2026-10-24, the llm-switch consumer was built on Linux with HuxerUI 0.3.0 and Cubism Native 5 R5. The Linux GDK context requests OpenGL ES 3.1, matching HuxerUI's `GL_RGBA8` external-texture contract. A Linux-only patch is applied to the extracted SDK cache at configure time: model vertex/index data and offscreen quad data are uploaded to VBO/EBO buffers instead of using client-side arrays, which ES 3 rejects. The wrapper also links the OpenGL library explicitly.

The full `LLMSWITCH_LIVE2D=ON` application build passed, as did all nine llm-switch CTest targets. The application was launched with an isolated data directory and the Live2D pet enabled; `ModelEvents::Ready` was observed within 16 seconds, with no playback failure diagnostic. This verifies successful first-frame rendering on the host. No screenshot, pixel comparison, transparency-edge check, or hit-area input check was performed.

## Other platforms

iOS arm64 Simulator builds passed, including Core and shader packaging. The later orientation check is recorded below; broader runtime and input checks remain pending. Windows and Linux have no corresponding host builds or runtime checks. Refer to the [user guide](../README.md) and [SDK notes](../docs/cubism-sdk.md) for platform and toolchain constraints.

## Multiple-character preview

The preview now bundles Mao, Haru, and Hiyori, with Mao selected initially. macOS, Android, and Web builds passed, as did Web type checking and Core tests.

Browser checks confirmed all three characters, Mao expression changes, rapid switching ending on the selected character, and layout changes between 360×640 and desktop sizes. Final checks did not reproduce duplicate-controller attachment or Web shader cleanup errors.

The updated Android APK was installed and Mao was confirmed visible on the physical device. The newer Haru/Hiyori device visuals remain unverified; browser results do not substitute for them. Loading stutter was reported at this stage. Investigation found duplicated Native PNG decoding and synchronous model/GPU preparation, leading to the changes below.

## Loading responsiveness

Native PNG decoding now runs through `RunWorker`. Assets retain decoded images for reuse by Metal, OpenGL, and D3D uploads. Each worker owns its input bytes and limits and does not access an asset reference, UI state, or the Cubism global lock. A started PNG decode can finish after cancellation, but its result is not delivered and later images are not started for that canceled task.

Player preparation is asynchronous internally; the public API did not change. Android retains an EGL context through the Cubism runtime and uses a `WorkerSequence` for shader preparation. The worker explicitly binds and releases the context; rendering begins only after preparation. Canceling a waiter does not interrupt compilation already executing inside the SDK. A runtime lease covers the worker and its cleanup, preventing premature SDK destruction or races with a new runtime acquisition.

Phase samples from the Huawei TAS-AN00 Debug build:

| Phase | Before | After |
| --- | --- | --- |
| Mao validation PNG decode | About 646 ms on the UI thread | Moved to workers; duplicate upload-time decode removed |
| Model metadata | About 6 ms | Remains serialized on the UI thread |
| Surface creation | About 5,357 ms on the UI thread | About 30 ms after shader preparation |
| First draw | About 51 ms | Still has synchronous work |
| Shader preparation | Included in surface creation | About 14,323 ms on another thread in one high-load run |

These samples came from different runs and are not a controlled performance benchmark or complete loading-time comparison. The evidence is that expensive shader compilation moved off the UI thread and uploads no longer decode PNGs again. The slower high-load worker sample must not be presented as an overall speedup. Temporary timing logs were removed.

Native Core/Metal integration tests passed after the change, including the added cancellation/remount cases. Web build, Mao display, and repeated switching regressions passed. Android interaction and pixel regression checks remained blocked by a locked device. Other Native platforms did not receive background GPU initialization in this change.

## Immersive interface

The preview replaced its large heading, persistent control cards, and desktop sidebar with a stable character stage, a floating bottom toolbar, and an optional adjustment panel. It keeps one controller and changes layout without reloading the model when width changes.

macOS, Android, and Web builds passed, and the updated Android APK was installed. Browser checks at 360×640 and 1280×720 confirmed the layout. Mao's visible height on the narrow screen increased from roughly 200 to 420 pixels. Opening the panel preserves character size, choosing an expression closes the panel, and resizing while paused preserves pause and expression state. These are screenshot observations; actual size depends on model canvas and motion. Updated Android device visuals still await verification.

## macOS texture orientation

The native macOS window initially displayed characters upside down. Temporary readback of the completed Metal render target showed Mao upright when pixels were read from the first row to the last. Publishing that source as `TopLeft` nevertheless produced an inverted native presentation.

The macOS publication boundary now uses `BottomLeft` to compensate for the installed HuxerUI importer's behavior. Model projection, hit coordinates, and, at that stage, iOS's `TopLeft` setting were preserved. This is a compatibility workaround, not a claim that the source texture's first row is actually the bottom. Retest and consider removing it when updating the SDK.

The repaired macOS build, Native Core/Metal integration test, and host-contract test passed. Computer-control permissions were unavailable, so the user checked the native window and confirmed Mao, Haru, and Hiyori were upright with working taps. Temporary readback code was removed; the production path gained no CPU pixel copy.

## Independent platform backends

Metal surfaces were split into `platform/ios/src/metal_surface.mm` and `platform/macos/src/metal_surface.mm`, removing the shared Apple file and platform macros. CMake selects platform sources and applies ARC. macOS arm64 and iOS arm64 Simulator builds passed, with logs confirming the matching source file for each target. Native Core/Metal integration passed on macOS. No rendering behavior was changed by that split; iOS visual acceptance had not been performed at that stage.

GL surfaces were split into `platform/android/src/main/cpp/gl_surface.cpp` and `platform/linux/src/gl_surface.cpp`. The shared GL file and Android/Linux macros were removed in favor of the root CMake platform source collection rules. Android retains the EGL runtime lease, worker shader preparation, and per-frame target allocation. Linux retains its GDK context, VAO, and resize-dependent target allocation. Unused shared-branch parameters, state, and constant conditions were removed.

The Android Debug APK built successfully, and its compile database contained only the Android GL implementation. Linux was compared against its original branch but was not built or run. Device visuals were not repeated for this source-organization change.

## Bundled preview model resources

Mao, Haru, and Hiyori now reside directly in `examples/preview/resources/raw/`. All 87 model files were compared byte-for-byte with their Native 5 R5 originals, and the upstream license notice was copied unchanged beside an English source notice. CMake no longer copies sample models from the SDK cache or registers a separate model namespace. The preview references the automatically generated `app_resources.h` identifiers directly; the authored sample resource header was removed.

macOS, Android, Web, and iOS arm64 Simulator builds passed after migration. All 87 typed references resolve to local model files. The macOS bundle contains identical model bytes under `app/raw`, and the Android APK includes all three manifests under that namespace; neither checked package retains the old model namespace. This verifies resource migration and packaging, not a new visual or input acceptance run.

## Preview localization

The preview uses generated string resources for English fallback and Simplified Chinese, including formatted motion names, status messages, errors, and accessibility labels. macOS, Android, and Web builds passed after this change.

A temporary test using the public HuxerUI `UiTest` API ran the actual preview with its packaged resources and real Core/Metal backend. It checked English, `zh-CN`, and English fallback for `fr-FR`; translated an active motion status; preserved pause state; and confirmed that locale changes did not reopen a model manifest. All three characters loaded, including Hiyori's localized empty-expression message. Control bounds were checked at 360×640 and 1280×720. These were semantic and layout-bound assertions, not screenshot or pixel acceptance checks. Android, Web, and iOS runtime locale changes were not separately verified.

Japanese was subsequently added in `ja.properties`. All 32 keys and formatting placeholders match the English fallback catalog, and the macOS resource generation and build passed. Japanese runtime layout and device display have not been separately checked.

## iOS texture orientation

The user reported an upside-down Haru in the iPhone 17 Pro simulator running iOS 26.5. The independent iOS Metal backend now publishes frames as `BottomLeft`, applying the same publication-boundary workaround used on macOS. Projection and hit coordinates were unchanged.

The updated iOS arm64 Simulator build passed and was installed and launched. A screenshot captured with `simctl` confirmed Mao was upright with ready status. Computer-control permissions were unavailable for interaction checks, so Haru/Hiyori switching and hit-area input after the fix remain unverified. Physical iOS devices have not been checked. Recheck the workaround when upgrading HuxerUI.

## File and raw-resource loading

The public loader now has a `File` overload that opens dependencies relative to the manifest's parent directory and a `RawResource` overload with an application-provided opener. Both use one private package-loading implementation; the string-entry overload is no longer public. The preview passes generated model entry identifiers directly.

The macOS Native integration test passed using the real Wanko package through both entry points. It checks file loading with texture and motion subdirectories, raw-root-relative callback paths for a nested entry, matching parameter counts, original missing-file errors, byte limits, invalid raw keys, invalid configuration, and traversal rejection for both sources. Existing Core/Metal publication, controller, cancellation, and remount checks also passed.

macOS, Android, Web, and iOS arm64 Simulator builds passed. The README's file and bundled-resource examples and the header's file-loading example compiled against the installed SDK; Doxygen reported no documentation warnings. This change did not repeat device visual or input acceptance checks.

## Release review regressions

The Native integration test now verifies that a two-second Wanko motion completes within 19 frames at 60 FPS and an eightfold playback rate. Active frame time is capped before scaling, and neither backend truncates the scaled interval again.

The Web contract test compiles an in-memory test bundle that exposes bridge internals without changing the shipped API. It checks eight seconds of model-time advancement over 60 frames at eightfold speed, distinguishes invalid dependency paths from malformed manifests, and retires multiple shader contexts while another context and model asset remain alive. Deferred cleanup keeps a context registered while shaders are loading and removes it afterward. Cubism Web 5 R5 lacks a public per-context removal method, so the adapter accesses its pinned shader registry; rerun these checks when upgrading the SDK.

Native integration, host-contract tests, Web type checking and contract tests, and macOS, Android, Web, and iOS arm64 Simulator builds passed. Doxygen reported no documentation warnings. The Web timing and cleanup checks replace GPU calls and do not establish browser pixel or device-input correctness.
