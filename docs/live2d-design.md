# Live2D Integration Design

Status: the initial implementation is in place. This document describes the architecture, implemented behavior, and acceptance targets that remain unverified. It does not claim that every platform or edge case has passed validation.

Updated: 2026-09-10. The pinned Native/Web 5 R5 SDKs, public API, shared loader, and six platform backends are integrated. See the [user guide](../README.md) for application integration and the [validation records](../tests/README.md) for evidence.

The design originated from the HuxerUI Live2D integration proposal. Commands without a payload return `PlaybackError` directly; operations with a payload use `Result<T, Error>`.

## Goals and scope

Provide an independent HuxerUI library that displays Live2D models through ordinary layout, clipping, transforms, opacity, and event handling.

The initial scope includes model-package loading, independent instances, motions, expressions, automatic blinking and breathing, physics and pose updates, parameter overrides, hit areas, and pause/resume. Supported behavior depends on the pinned Cubism Core/Framework version, model features, and actual platform validation.

Face tracking, camera and microphone capture, audio playback, audio analysis, AI conversations, model editing, network downloading, archive extraction, a global model cache, and a background rendering thread are outside this scope. Applications can drive mouth parameters through parameter inputs.

The implementation does not promise identical pixels across platforms, compatibility with every historical model, or zero-copy publication.

## Public identity and HuxerUI integration

| Item | Identity |
| --- | --- |
| Public CMake target | `HuxerUI::Live2D` |
| Implementation target | `huxerui_live2d` |
| C++ namespace | `huxerui::live2d` |
| Public header | `<huxerui/live2d.h>` |
| Consumer application | `examples/preview` |

The generated `Install(RootContext&)` placeholder has been removed. There is no root installation service.

The implementation uses the installed SDK's public contracts:

- `Image` accepts `std::shared_ptr<ExternalTexture>`.
- An external texture has an immutable logical size. Publishing a frame does not require recomposition or re-recording the paint sequence.
- `NodeExtension` provides mounted lifetime, frame processing, geometry preparation, pointer handling, and typed events.
- `RawAsset::OpenReadAsync()` returns `Task<AsyncInputStream>`. Opening can raise exceptions; subsequent stream operations use `IoResult`.
- Apple `MetalTexture::Publish()` requires completed producer writes and synchronously copies a texture snapshot.

No HuxerUI private APIs, new Runtime subclasses, node types, paint commands, or public GPU abstractions are introduced.

## Objects and ownership

| Object | Responsibility | Ownership |
| --- | --- | --- |
| `ModelAsset` | Model files, decoded Native images, metadata, and a runtime lease | Cheap shared handle; no per-instance playback state |
| `ModelController` | One-shot motion and expression commands | Shared command identity, weakly attached to one mounted binding |
| `ModelView` | Declarative display and persistent options | Returns a normal `View`; retains composition state internally |
| Private `Player` / `NativeModel` | Mutable model, animation, physics, renderer, and hit testing | Owned through the mounted extension's binding |
| Private `PlatformSurface` | Graphics context, model textures, render target, and publication | Owned by the player, with platform-specific context sharing |

One asset can create multiple independent players. Ordinary recomposition preserves the player, motion time, and texture identity when both asset and controller identity remain stable. Replacing either asset or controller currently recreates the binding and player.

Cubism initialization is managed through a shared internal runtime lease. Shutdown occurs after the last dependent asset, player, or preparation task releases that lease. The current Native implementation assumes one Cubism provider in the process and rejects a separately initialized provider.

Android's runtime also owns a reusable EGL context. An asset can therefore indirectly retain shared graphics infrastructure through its runtime lease, even though it does not own a per-instance render target or model textures.

## Public API

The following excerpts omit unrelated declarations. The [public header](../include/huxerui/live2d.h) is authoritative.

### Loading

```cpp
using OpenResourceAsync =
    std::function<Task<IoResult<AsyncInputStream>>(std::string relative_path)>;

Task<Result<ModelAsset, LoadError>> LoadModelAsync(
    File entry_file,
    LoadLimits limits = {});

Task<Result<ModelAsset, LoadError>> LoadModelAsync(
    RawResource entry_resource,
    OpenResourceAsync open_resource,
    LoadLimits limits = {});
```

The File overload uses the manifest's parent directory as the package root and opens relative dependencies through File::OpenReadAsync(). The RawResource overload removes the entry key's `raw/` prefix and passes normalized raw-root-relative paths to the supplied opener, which must access the entry's resource domain. File paths and resource identities are passed by value so asynchronous work can own them. Both overloads use one private package loader for validation, dependency reads, and asset preparation. The loader retains the opener and its captured resource access until completion or cancellation cleanup. Each open request must return an independently owned stream positioned at the beginning.

Loading runs within HuxerUI's task execution environment and uses its cancellation behavior. Success means required dependencies and asset preparation completed; it does not mean a mounted instance has prepared its GPU resources or published a frame. Native PNG decoding happens during loading, while GPU upload occurs during player preparation. Web decoding and graphics preparation follow the Web bridge's lifecycle.

Metadata includes motion groups and counts, expression names, parameter IDs and ranges, hit-area names, and canvas size. Names come from the model rather than a hardcoded vocabulary.

### Declarative display

```cpp
struct ModelOptions {
  std::optional<ModelController> controller;
  ImageFit fit = ImageFit::Contain;
  bool paused = false;
  float playback_rate = 1.0F;
  bool auto_blink = true;
  bool auto_breath = true;
  std::vector<ParameterInput> parameters;
};

View ModelView(ModelAsset asset, ModelOptions options = {});
```

The component uses a configuration structure. Its implementation is marked `[[huxerui::composable]]` where composition state is required.

```cpp
return ModelView(asset, {
    .controller = controller,
    .paused = paused,
})
    .On<ModelEvents::Ready>(on_ready)
    .On<ModelEvents::HitAreaTapped>(on_hit)
    .With(Frame{.width = 320.0F, .height = 480.0F});
```

The application owns loading, errors, and placeholder UI. Persistent options are the source of truth; there are no competing controller setters for pause, rate, or automatic effects. Applications that issue commands should retain a controller across recompositions. Omitting it creates a stable internal controller.

### Commands and errors

```cpp
class ModelController {
public:
  Result<PlaybackId, PlaybackError> PlayMotion(
      std::string_view group, int index, MotionOptions options = {}) const;

  PlaybackError StopMotion() const;
  PlaybackError SetExpression(std::string_view name) const;
  PlaybackError ClearExpression() const;
};
```

`PlaybackError` contains an error code and an English diagnostic. The default `PlaybackErrorCode::None` represents success. `Result` failure branches and failure events must contain a non-success error. `LoadError` additionally carries a package path and an optional underlying `IoError`.

Command errors include `NotAttached`, `NotReady`, `UnknownMotion`, `UnknownExpression`, `PriorityRejected`, `UnsupportedBackend`, and `BackendFailed`. Configuration errors, such as non-positive motion priority, use exceptions rather than operational error values.

Commands execute on the mounted UI thread; calls from another thread throw. Names are consumed synchronously and are not borrowed after return. Unattached or preparing controllers reject commands immediately rather than queueing them.

Successful `PlayMotion` returns an opaque `PlaybackId` indicating acceptance, not completion. IDs increase within a controller identity across remounts and asset changes. `MotionOptions` exposes positive priority, forced replacement, and looping. Fade behavior comes from the model and pinned Cubism SDK; there is no public fade-duration option.

Commands are processed directly with serialized Native access. There is no permanent command queue. Any future deferred queue would need explicit ordering, bounds, and overflow behavior.

### Events

| Event | Meaning |
| --- | --- |
| `ModelEvents::Ready` | The binding published its first displayable frame and can accept commands |
| `ModelEvents::MotionFinished` | Playback ID and `Completed`, `Interrupted`, or `Stopped` |
| `ModelEvents::HitAreaTapped` | A model-defined hit-area name |
| `ModelEvents::PlaybackFailed` | An operational or backend failure after mounting |

Finishing an iteration of a looping motion is not completion of the whole playback. Synchronously rejected commands return errors without duplicate failure events.

Events use HuxerUI's `Event<void(...)>` and `.On<Key>()`. They are emitted from allowed frame or input phases after internal state is consistent, rather than from construction, configuration reconciliation, geometry preparation, painting, or destruction. Cubism results are collected before application callbacks run.

Callbacks can issue further commands. Internal locks must not prevent that reentrancy, and code must not assume earlier state remains valid after a callback. No events are sent to an unmounted node; outstanding motion-completion events are not guaranteed during teardown.

## Package structure and loading

A package consists of a `.model3.json` manifest and its referenced moc, textures, motions, expressions, physics, pose, and supported auxiliary files. Missing declared dependencies fail loading. Undeclared optional features remain absent. Audio playback is outside the loader's feature scope.

The preview stores the original Mao, Haru, and Hiyori directories under `examples/preview/resources/raw/`, together with upstream license notices. HuxerUI packages them in namespace `app` and generates `app_resources.h`. The preview's loading code references those typed `app::raw` identifiers directly. Because the package root is the raw resource root, it removes the known `raw/` prefix from each logical key to populate the package-relative lookup map and passes the identifier to `UseRawResource`. This mapping does not resolve local filesystem paths. No separate sample resource header or SDK-to-preview copy step is needed. A `RawAsset` is not a directory resolver.

Model textures remain original raw files for Cubism. Native decoding currently accepts PNG. Package-relative paths preserve case and UTF-8, normalize `.` and `..`, and reject absolute paths, NUL, backslashes, colons, and traversal outside the root. A filesystem adapter handling untrusted packages must separately prevent symlink escape.

Openers execute in the loading task's environment. They should use public asynchronous file or raw-resource APIs and translate recoverable open exceptions to `IoResult`. Web code must yield for asynchronous operations rather than synchronously wait for promises.

Default limits are 64 MiB per file, 256 MiB total input bytes, 1,024 files, and 8,192 pixels per texture dimension. Native preparation also checks decoded-image budgets, positive canvas dimensions, and a drawable-count limit. Not every resource-related rejection maps to `ResourceLimit`: Native image-validation exceptions currently become `InvalidModel`.

Cubism APIs that require contiguous or aligned data receive owned buffers with the required lifetime. Streaming I/O does not imply incremental Cubism parsing. Cancellation suppresses delivery through HuxerUI task semantics and is not converted into a normal load error.

## Mounting, texture connection, and teardown

Composition creates lightweight binding state and view descriptions. It does not read model files, initialize a player, allocate GPU resources, or start playback.

A retained extension owns the binding. Once positive geometry is available, `OnFrame` starts asynchronous player preparation. The preparation task captures the asset and a weak binding; canceled or replaced bindings cannot receive late results. On Native platforms, decoded images are reused for texture upload. Android warms shaders on a `WorkerSequence` with an explicitly current EGL context. That worker holds a runtime lease until completion, including cancellation cleanup.

Before texture connection, a stable `Stack` host contains a placeholder sized from the asset canvas. After successful publication, a posted update supplies the texture to an ordinary `Image` without replacing the host extension. State changes connect textures or wake idle work; animation frames update through texture publication rather than incrementing composition state.

```text
Mounted -> Preparing -> Ready
                    -> Failed
Asset/controller replacement -> new binding -> Preparing
Any mounted state -> Unmounted
```

Binding identity invalidates old preparation and connection work. Resize alone does not recreate the binding or re-emit `Ready`. Future graphics recovery would need a distinct readiness cycle; automatic recovery is not currently implemented.

Unmounting cancels preparation, releases the binding and controller attachment, and destroys graphics objects in the appropriate context. Cleanup must preserve snapshots already retained by HuxerUI and must not synchronously wait for work that requires the same UI thread to continue.

Backend failures are reported once for the failed binding rather than retried every frame. Recovery currently requires remounting. Retaining assets across recoverable device loss and rebuilding surfaces automatically remains an acceptance target.

## Time, pause, and scheduling

The extension uses `FrameInfo`, returns `FrameResult`, and tracks whether time was advancing. Resume establishes a fresh baseline rather than replaying a long inactive interval.

A paused model or zero playback rate freezes motion, physics, blinking, and breathing. Playback rate must be finite and between zero and eight. Dirty configuration or geometry can still request a single render while time is frozen. Commands accepted while paused can change state without advancing time.

Successful commands notify local composition state to wake the retained host. Geometry changes also request a later update. The acceptance target is no continuous frame requests for an unchanged paused instance, while idle commands and size changes still produce visible updates. Forced `UiTest::Pump()` calls alone cannot prove platform idle wake behavior.

Active frame time is clamped to 0.1 seconds before applying the playback-rate multiplier. Native and Web players consume that complete scaled interval, so high playback rates are not truncated. Physics stepping uses the SDK's update behavior; independent physics-settling detection and arbitrary seek are not implemented.

The current reduced-motion policy freezes all automatic time advancement, including explicitly requested motions. Necessary dirty renders can still occur. This implementation detail is stricter than the original goal of suppressing only decorative autonomous animation and needs separate usability review before a broader promise is made.

`ExternalTexture::IsActive()` is not used to promise automatic offscreen pause/resume. Initial inactivity must not suppress the first frame.

## Rendering, geometry, and input

```text
Cubism model update
    -> transparent offscreen target preserving the canvas aspect ratio
    -> platform ExternalTexture publication
    -> Image fit, layout, clipping, transforms, and opacity
```

Cubism owns internal draw order, masks, and blending. Blend operations apply within the model's offscreen surface; they are not guaranteed to blend directly against arbitrary HuxerUI content behind it.

Texture logical size comes from the model canvas. The current physical target is estimated at twice the fitted display size, capped at 4,096 pixels on the longest side. This is not actual display-density integration. `Image` performs fitting; the backend does not duplicate clipping to the view rectangle.

Zero-sized geometry does not render and cannot become ready. Most backends reuse targets until size changes. Android intentionally allocates fresh texture storage each frame to detach the previous EGLImage before publication.

`PrepareGeometry` records content bounds and schedules updates; `OnFrame` advances and publishes. Strict alignment between input geometry, model state, and the exact sampled external-texture frame remains a validation target, including any one-frame resolution change.

Frames clear to transparent and declare their format, origin, and alpha convention. Producer writes finish before publication, and snapshots remain immutable for the consumer. Publication may copy and block; it is not a zero-copy contract. Semi-transparent edges need checks on light and dark backgrounds, especially for complex blend models.

The current macOS and iOS HuxerUI importers display these Metal frames upside down when published as `TopLeft`. Both independent backends compensate using `BottomLeft` metadata at publication; raw readback on macOS showed that the source pixels themselves were upright. This workaround must be rechecked when updating the SDK rather than treated as a general Metal coordinate rule.

Input follows the normal Runtime route. A primary-pointer tap is canceled after movement beyond eight logical units or a cancel event. Hit testing maps the fitted image rectangle to normalized model coordinates and uses model-defined drawable bounds. No hardcoded motion is automatically played by the library. Hit-area passthrough, disabled/cancel interactions, and exact displayed-frame consistency still require broader validation. Continuous gaze tracking and dragging are outside the initial scope.

Models are decorative by default. Applications should supply useful semantic labels and actions without creating accessibility nodes for every mesh or parameter.

## Parameter overrides

`ParameterInput` takes an exact model parameter ID, a finite value, a blend mode, and a weight in `[0, 1]`. Duplicate or unknown IDs and invalid values are configuration errors.

Overrides are applied after the normal motion, effect, physics, and pose stages, before final model evaluation:

- `Override`: `current * (1 - weight) + value * weight`.
- `Add`: `current + value * weight`.
- Clamp the result to the parameter's metadata range.

Overrides affect display and are not promised as physics inputs. They must not overwrite the saved base parameters used by subsequent motion updates. Repeated paused evaluation must not accumulate `Add` inputs. Removing an override restores the base evaluation result.

The Native implementation retains evaluated base parameters for this purpose. Native/Web save-and-restore ordering, expressions, blinking, physics, and pose still need comprehensive tests against the pinned SDK. No arbitrary callback exposes a half-updated Cubism model.

## Source and platform organization

```text
include/huxerui/live2d.h
src/
    model.cpp                    Asset loading, paths, and controller commands
    model_view.cpp               Composition and retained extension
    native/                      Shared Native model behavior and image decoding
platform/
    android/src/main/cpp/         EGL/GLES backend and shader preparation
    ios/src/                     iOS Metal backend
    macos/src/                   macOS Metal backend
    linux/src/                   GDK/OpenGL backend
    windows/src/                 D3D11 backend
    web/                         Web bridge and Web player
examples/preview/                Public-API consumer and sample resources
```

Platform rendering implementations are independent. `NativeModel`, `NativePlayer`, and `src/native/` identify the shared Cubism Native SDK implementation, distinct from the Web SDK implementation. `PlatformSurface` and `CreatePlatformSurface` select the platform graphics implementation without changing that shared model behavior. There are no shared Apple or Android/Linux surface files. CMake selects platform sources and links the matching Core and renderer dependencies. Shader build helpers remain in `cmake/`.

| Platform | Rendering path | Main validation concerns |
| --- | --- | --- |
| macOS | Native Metal -> `macos::MetalTexture` | AppKit import, origin, Core architecture, synchronization and copy cost |
| Android | Native GLES -> `android::GlTexture` | EGL lifetime, ABI, alpha, context loss |
| Web | Cubism Web/WebGL canvas -> `web::VideoFrameTexture` | Canvas capture, alpha, browser APIs, JS/WASM lifetime |
| iOS | Native Metal -> `ios::MetalTexture` | Device/Simulator slices, lifetime, final bundle resources |
| Windows | Native D3D11 -> `windows::D3D11Texture` | Same adapter, supported formats, device loss |
| Linux | Native OpenGL -> `linux::GlTexture` | GDK context/share groups, thread ownership, Core architecture |

Web uses the separate JavaScript Core rather than recompiling Native Core into application WASM. Unsupported required capabilities, such as missing `VideoFrame`, produce an explicit failure. No fallback embeds a platform view.

Graphics operations execute serially in a valid owning context. Android's dedicated preparation sequence explicitly binds and releases EGL; an ordinary worker pool is not used for thread-affine rendering. Public headers expose no Cubism, GL, Metal, D3D, or JavaScript objects.

Existing `huxerui_add_library`, `huxerui_use_library`, and platform packaging conventions remain in use. Cubism is an implementation dependency, but final linking and shader deployment must reach the consumer application. iOS builds combine the wrapper, Framework, and Core archives and stage Metal libraries for the Xcode bundle resource phase.

## Validation targets

The initial validation sequence was macOS, then Android and Web, followed by the remaining hosts. Available build, runtime, and image evidence is recorded separately in [tests/README.md](../tests/README.md).

| Area | Acceptance targets |
| --- | --- |
| Loading | Relative dependencies, missing files, resource limits, traversal, cancellation, resource mapping |
| Ownership | Multiple instances, recomposition preserving time, controller replacement/duplicate binding, stable texture connection |
| Scheduling | Idle wakeup, no continuous paused frames, paused parameter/size updates, resume without time jumps |
| Commands | Success/error values, attachment/readiness rejection, priorities, loops, interruptions, playback IDs, callback reentrancy |
| Parameters | Ordering, clamping, duplicate/unknown IDs, restoring removed overrides, non-accumulating paused Add |
| Input | Contain margins, cover clipping, transforms, scale, passthrough, cancellation, displayed-frame consistency |
| Rendering | Alpha, orientation, masks/blending, density/size, context loss, late publication |
| Cleanup | Unmount during preparation, stale asset tasks, correct-context destruction, no post-unmount events |
| Packaging | Public-header-only consumers, pinned SDK, architecture, static linking, runtime resources |

Use controlled clocks and random seeds for deterministic timing tests where possible. GPU pixels require rendered output; semantic snapshots cannot validate external-texture orientation or alpha. Record model-update time, GPU time, publication copy/blocking, memory, and command-to-visible-frame latency separately rather than making unmeasured performance claims.

Open work includes real idle wakeup measurements, exact hit/display synchronization, multi-instance pressure, complex masks and blending, alpha error, platform device loss, older Apple OS support, and remaining host coverage. PNG decoding is already off the UI thread and Android shader compilation is prepared in the background; Native metadata/model creation, texture upload, first draw, and non-Android graphics setup still have synchronous work. No frame-publication performance budget has been established.

If validation requires a new HuxerUI capability, propose the smallest public contract separately. Do not expand this library into a Runtime or resource-system refactor.

## References

- [Cubism platform support](https://docs.live2d.com/en/cubism-sdk-manual/platform/)
- [Cubism Native Framework](https://github.com/Live2D/CubismNativeFramework)
- [Cubism Native Samples](https://github.com/Live2D/CubismNativeSamples)
- [Cubism hit areas](https://docs.live2d.com/en/cubism-sdk-manual/hitarea/)
- [Cubism Web SDK](https://www.live2d.com/en/sdk/download/web/)
- [Cubism SDK license](https://www.live2d.com/en/sdk/license/)

Use the pinned SDK's files and license texts when implementing or distributing it. The original proposal and platform table do not replace testing or grant redistribution rights.
