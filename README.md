# HuxerUI Live2D

This downstream fork uses `farna/main` as its integration branch. The initial runtime matches the official `936694b` baseline without additional runtime changes. The official baseline is tracked through `upstream/main` from `HuxerUI/Lib-Live2D`. Consumers should pin a validated commit; the Cubism SDK remains an external local prerequisite and is not redistributed by this fork.

Display and animate Live2D characters in a HuxerUI application. Load a Cubism `.model3.json` package, place a `ModelView` in your layout, and control motions and expressions through a `ModelController`.

The library supports independent character instances, motion playback, expressions, automatic blinking and breathing, physics and pose updates, parameter overrides, hit areas, and pause/resume. It integrates with normal HuxerUI sizing, clipping, transforms, and opacity.

This is an early implementation. The preview includes three official sample characters: Mao, Haru, and Hiyori.

## Platform support

| Platform | Backend | Validation status |
| --- | --- | --- |
| macOS arm64 | Metal | Built; integration tests and three-character display/input checks passed |
| Android arm64 | OpenGL ES / EGL | Built; Wanko and Mao displayed on a physical device; latest loading/UI changes still need device regression checks |
| Web | Cubism Web / WebGL / VideoFrame | Built; three-character display, switching, expressions, and responsive layout checked in a browser |
| iOS arm64 Simulator | Metal | Built and launched; Mao orientation checked in a simulator screenshot; switching/input checks pending |
| Windows | D3D11 | Implemented; build and runtime checks pending |
| Linux x86_64 | OpenGL / GDK | Implemented; build and runtime checks pending |

See [validation records](tests/README.md) for exact coverage. Older Apple OS compatibility is not established for the pinned Core binaries; see [SDK compatibility](docs/cubism-sdk.md#toolchains-and-compatibility).

## Requirements

- HuxerUI SDK and CLI, tested with version 0.3.0.
- A C++20 toolchain, CMake, and the platform tools required by HuxerUI.
- Python 3 for downloading the pinned Cubism SDKs.
- Cubism Native and/or Web 5 R5 (`5-r.5`).
- A model package you have permission to use, including its `.model3.json` and every referenced dependency.

Android builds use JDK 17. Apple builds require Xcode with the Metal compiler. Linux integration requires GTK 4.6+ and GLEW. Web builds also require Node.js, npm, Emscripten, and browser support for `VideoFrame`.

## Try the preview

From this repository's root, download the dependencies:

```sh
python3 tools/fetch_cubism.py
```

The script verifies pinned archive hashes and extracts both SDKs into `.cache/cubism`. The preview already includes Mao, Haru, and Hiyori under `examples/preview/resources/raw/`; builds package these local files directly and do not copy models from the SDK cache.

Build or run the preview with HuxerUI:

```sh
huxerui run macos
huxerui build android --java-home /path/to/jdk17
huxerui devices ios
huxerui build ios --device <simulator-id>
```

For Web, install the additional build dependencies in this repository first:

```sh
npm install --prefix .cache/web-tools --no-audit --no-fund esbuild@0.25.12 typescript@5.9.3
huxerui run web
```

Use the bottom toolbar to switch characters, pause playback, or trigger an interaction. The adjustment panel exposes the selected model's motions and expressions. Controls adapt to narrow and wide windows.

The preview follows the system language, with English as the fallback and Simplified Chinese and Japanese translations. Interface text lives in `examples/preview/resources/strings/default.properties`, `zh.properties`, and `ja.properties`, accessed through the generated `app::strings` identifiers. Add a locale catalog with the same keys to provide another translation. Language changes preserve the loaded character and playback state. The window title stays `Live2D`; model identifiers and SDK diagnostic details retain their original text.

## Add the library to your application

In your application's CMake file, after creating `my_app`:

```cmake
huxerui_use_library(my_app
    TARGET HuxerUI::Live2D
    URL "https://github.com/HuxerUI/Lib-Live2D.git"
    TAG "v0.1.0"
)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY AND (APPLE OR EMSCRIPTEN))
    live2d_configure_app(my_app)
endif ()
```

Include `<huxerui/live2d.h>` and use the `huxerui::live2d` namespace. No `Install()` call is required.

The URL dependency fetches the wrapper, not the Cubism SDKs. Before configuring the application, set `LIVE2D_CUBISM_NATIVE_ROOT` for Native builds or `LIVE2D_CUBISM_WEB_ROOT` for Web builds to an extracted Cubism 5 R5 SDK. These are CMake cache variables; without overrides, they point into the fetched library's `.cache/cubism`. Web builds also require the npm build dependencies described above in the fetched library directory.

Shader deployment depends on the platform:

| Platform | Application setup |
| --- | --- |
| macOS | `live2d_configure_app` adds `FrameworkMetallibs` to the application bundle |
| iOS | The helper stages `live2d-ios/FrameworkMetallibs` under the CMake build directory; copy this directory to the final Xcode application bundle's root in a resource build phase |
| Web | The helper places `live2d-shaders` beside the generated application JavaScript; deploy that directory with the application |
| Android / Linux / Windows | No additional application shader-copy step is required by this library |

The [preview CMake configuration](examples/preview/CMakeLists.txt) and [iOS project](examples/preview/platform/ios) show the complete packaging setup.

## Load and display a model

For a model already available in the application's local file system, pass its manifest as a `File` from a loading task:

```cpp
auto result = co_await LoadModelAsync(File("/path/to/Character/Character.model3.json"));
```

The loader uses the manifest's parent directory as the package root and opens declared dependencies automatically. Dependencies must stay lexically within that root; symbolic links follow normal `File` behavior. On Web, `File` refers to the virtual file system. File I/O failures retain their original error in `LoadError::cause`.

For HuxerUI's bundled resources, pass a generated `RawResource` plus a callback that opens the manifest and its dependencies. The current resource API does not provide relative lookup from a single raw asset, so this entry point requires a resource mapping:

Keep the model's original directory structure under your application's raw resources. For example, a minimal package could contain:

```text
resources/raw/Character/
    Character.model3.json
    Character.moc3
    textures/texture_00.png
```

Use the actual names from your model's manifest. Also package and map every referenced motion, expression, physics, pose, display-info, and user-data file. Model textures must remain raw files; the Native backend decodes PNG textures.

The following component assumes your generated application's resource namespace is `app` and the package uses the three paths above. Include HuxerUI's generated `app_resources.h` and pass its typed `app::raw` identifiers directly to `UseRawResource`. Add the component to a source file included in your HuxerUI application target, extend `resources` using the actual identifiers in the generated header, and call `CharacterView()` from your application's view tree. The map keys remain the package-relative paths requested by Live2D; they are distinct from the generated C++ identifiers.

```cpp
#include <huxerui/huxerui.h>
#include <huxerui/live2d.h>
#include "app_resources.h"
#include <map>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::live2d;

using ModelResources = std::map<std::string, RawAsset>;

Task<IoResult<AsyncInputStream>> OpenModelResource(ModelResources resources, std::string path) {
  const auto found = resources.find(path);
  if (found == resources.end()) {
    co_return IoResult<AsyncInputStream>::Failure({IoErrorCode::NotFound, path});
  }
  try {
    co_return IoResult<AsyncInputStream>::Success(co_await found->second.OpenReadAsync());
  } catch (const std::runtime_error& error) {
    co_return IoResult<AsyncInputStream>::Failure({IoErrorCode::Io, error.what()});
  }
}

Task<void> LoadCharacter(ModelResources resources, State<ModelAsset> asset, State<std::string> status) {
  auto result = co_await LoadModelAsync(app::raw::Character_Character_model3_json, [resources](std::string path) {
    return OpenModelResource(resources, std::move(path));
  });
  if (!result.Succeeded()) {
    status = result.Error().path + ": " + result.Error().message;
    co_return;
  }
  asset = std::move(result).Value();
  status = "Preparing graphics...";
}

[[huxerui::composable]]
View CharacterView() {
  auto asset = UseState(ModelAsset{});
  auto status = UseState(std::string("Loading model..."));
  auto paused = UseState(false);
  const ModelController controller = UseState(ModelController{});
  auto tasks = UseTaskScope();

  ModelResources resources{
    {"Character/Character.model3.json", UseRawResource(app::raw::Character_Character_model3_json)},
    {"Character/Character.moc3", UseRawResource(app::raw::Character_Character_moc3)},
    {"Character/textures/texture_00.png", UseRawResource(app::raw::Character_textures_texture_00_png)},
  };
  Lifecycle([tasks, resources, asset, status] {
    auto request = tasks.Launch(LoadCharacter(resources, asset, status));
    return [request] { request.Cancel(); };
  });

  if (!asset->HasValue()) return Text(status);
  const ModelAsset loaded = asset;
  return Column {
    ModelView(loaded, {.controller = controller, .paused = paused})
        .On<ModelEvents::Ready>([controller, loaded, status] {
          status = "Ready";
          for (const auto& group : loaded.Info().motion_groups) {
            if (group.count == 0) continue;
            auto result = controller.PlayMotion(group.name, 0, {.loop = true});
            if (!result.Succeeded()) status = result.Error().message;
            break;
          }
        })
        .On<ModelEvents::HitAreaTapped>([status](std::string name) { status = "Tapped: " + name; })
        .On<ModelEvents::PlaybackFailed>([status](PlaybackError error) { status = error.message; })
        .With(Frame{.width = 360.0F, .height = 480.0F}),
    Text(status),
    Button(paused ? "Resume" : "Pause").OnClick([paused] { paused = !paused; }),
  }.With(Spacing(8.0F));
}
```

The example loops the first available motion; select an idle group explicitly if your model provides one. Loading succeeds before GPU preparation finishes: wait for `ModelEvents::Ready` before sending playback commands.

The `RawResource` overload accepts an `OpenResourceAsync` callback. It receives normalized paths relative to the entry domain's raw root, without the `raw/` prefix, and must return a fresh stream for each request. For the example above, the first request is `Character/Character.model3.json`; texture requests include `Character/textures/texture_00.png`. The callback is responsible for accessing the same resource domain as the entry. Convert recoverable storage-opening exceptions to `IoResult` in that adapter. Use `TaskScope` to own loading work; do not read whole model files during composition.

For model switching, retry handling, and a complete application, see the [preview source](examples/preview/src/app.cpp). It references `app::raw` identifiers from HuxerUI's automatically generated `app_resources.h`; no separate model resource header is required.

## Control playback

Retain a controller across recompositions and attach it to one `ModelView`. Reuse a loaded `ModelAsset` with separate controllers to display independent instances. Omitting the controller gives the view a stable internal controller.

Call commands on the mounted UI thread, usually from an event handler:

| Command | Result |
| --- | --- |
| `PlayMotion(group, index, options)` | `Result<PlaybackId, PlaybackError>`; success means playback was accepted |
| `StopMotion()` | `PlaybackError`; `code == PlaybackErrorCode::None` means success |
| `SetExpression(name)` | `PlaybackError` |
| `ClearExpression()` | `PlaybackError` |

Read `asset.Info().motion_groups`, `expressions`, `parameters`, and `hit_areas` to discover names and ranges. Motion indices start at zero. Names such as `Idle` and `TapBody` belong to particular models and are not universal.

`MotionOptions` defaults to priority `1`, `force = false`, and `loop = false`. Priority must be positive. A lower-priority request may return `PriorityRejected`; use `force = true` when your interaction should replace the current motion. Retain the returned `PlaybackId` when correlating completion events.

Persistent settings belong in `ModelOptions`:

| Option | Default | Usage |
| --- | --- | --- |
| `fit` | `ImageFit::Contain` | Fit the model into the view's bounds |
| `paused` | `false` | Freeze model time |
| `playback_rate` | `1.0F` | Finite value from `0` through `8`; zero freezes time |
| `auto_blink` / `auto_breath` | `true` | Enable model-supported automatic effects |
| `parameters` | Empty | Apply parameter values after the normal model update |

Each `ParameterInput` specifies an existing model parameter ID, a finite value, a blend mode (`Override` or `Add`), and a weight in `[0, 1]`. IDs must be unique. Results are clamped to the model's declared range. These overrides can drive mouth parameters, but audio analysis and lip-sync input are application responsibilities.

## Handle events and errors

| Event | Meaning |
| --- | --- |
| `Ready` | The mounted instance published its first frame and accepts commands |
| `MotionFinished` | Reports the playback ID and `Completed`, `Interrupted`, or `Stopped` |
| `HitAreaTapped` | Reports a model-defined hit-area name |
| `PlaybackFailed` | Reports an operational error after mounting |

Load failures include a code, package path, diagnostic message, and an optional original `IoError`. Command failures are returned directly: an unattached controller returns `NotAttached`, and an instance preparing its first frame returns `NotReady`. Synchronous command rejection does not also emit `PlaybackFailed`.

Invalid configuration, such as duplicate parameter IDs, an out-of-range playback rate, or sharing a controller between mounted views, throws an exception. Canceling loading follows HuxerUI task cancellation rather than returning a normal load error.

A failed backend can be retried by remounting the model. Automatic device-loss recovery is not implemented. First-time graphics preparation may take time, especially on Android; show loading feedback until `Ready`.

## Further documentation

- [SDK setup and compatibility](docs/cubism-sdk.md)
- [Architecture and implementation limits](docs/live2d-design.md)
- [Tests and validation records](tests/README.md)
- [Public API](include/huxerui/live2d.h)

## License

The wrapper is available under the [MIT License](LICENSE). Cubism SDK components and sample models retain their own license terms; this repository's license does not cover them. The bundled models include [source and license notices](examples/preview/resources/raw/model_licenses/NOTICE.txt) and the [unmodified upstream license notice](examples/preview/resources/raw/model_licenses/CubismSdkForNative-LICENSE.txt). See the applicable model terms and downloaded SDK license files before distributing an application or model assets.
