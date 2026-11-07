#pragma once

/// @file
/// @brief Model loading, playback, and declarative Live2D views for HuxerUI.
/// Examples also include <huxerui/huxerui.h> and use the huxerui and huxerui::live2d namespaces.

#include <huxerui/data.h>
#include <huxerui/event.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>
#include <huxerui/view.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @brief Live2D assets, instance controls, options, and view events.
namespace huxerui::live2d {

namespace detail {
struct AssetData;
struct ControllerData;
struct Access;
}

/// @brief Categories of operational failure while loading a model package.
/// Cancellation follows the owning task scope and is not a load-error category.
enum class LoadErrorCode {
  /// Opening or reading a required resource failed; LoadError::cause may contain the original I/O error.
  Io,
  /// A package path is invalid or escapes the package root.
  InvalidPath,
  /// Model data or a required dependency failed parsing or validation, including PNG validation in the Cubism Native backend.
  InvalidModel,
  /// A checked file-count, byte, or model-geometry limit was exceeded.
  ResourceLimit,
  /// The selected SDK cannot support the model's format or required capabilities.
  UnsupportedModel,
  /// Required platform or runtime capabilities are unavailable for loading.
  UnsupportedBackend
};

/// @brief A model-loading failure with package context and an optional underlying storage error.
/// Inspect code for decisions; diagnostic strings are intended for people, not stable programmatic matching.
struct LoadError {
  /// Failure category; defaults to InvalidModel and has no success value.
  LoadErrorCode code = LoadErrorCode::InvalidModel;
  /// Related package-relative path, or an empty string when no path is available.
  std::string path;
  /// English diagnostic describing the failure; its exact wording is not an API contract.
  std::string message;
  /// Original opening or reading error when one was returned by the resource adapter or stream.
  std::optional<IoError> cause;
};

/// @brief Success and operational error categories for playback commands and mounted rendering.
enum class PlaybackErrorCode {
  /// The command succeeded; never used as a Result failure or PlaybackFailed event payload.
  None,
  /// The controller has no live mounted binding.
  NotAttached,
  /// The binding has not yet published its first frame.
  NotReady,
  /// The requested motion group or zero-based index does not exist.
  UnknownMotion,
  /// The requested expression name does not exist.
  UnknownExpression,
  /// Cubism rejected a motion request because of the current or reserved priority.
  PriorityRejected,
  /// Required rendering capabilities are unavailable on this backend.
  UnsupportedBackend,
  /// Model execution, graphics preparation, rendering, or frame publication failed.
  BackendFailed
};

/// @brief Status returned by commands that have no result payload.
/// A code of PlaybackErrorCode::None means success. Failed results and failure events contain a non-success code.
/// @see ModelController, ModelEvents::PlaybackFailed
struct PlaybackError {
  /// Command status; defaults to success.
  PlaybackErrorCode code = PlaybackErrorCode::None;
  /// English failure diagnostic; empty on success. Do not parse it as a stable error identifier.
  std::string message;
};

/// @brief A model parameter's declared range and default value, in that parameter's own units.
struct ParameterInfo {
  /// Exact, case-sensitive model parameter ID, such as ParamMouthOpenY when provided by the model.
  std::string id;
  /// Inclusive lower bound declared by the model.
  float minimum = 0.0F;
  /// Inclusive upper bound declared by the model.
  float maximum = 0.0F;
  /// Initial value declared by the model, not the value currently produced by animation.
  float default_value = 0.0F;
};

/// @brief A named motion group exposed by a loaded model.
struct MotionGroup {
  /// Exact group name from the manifest; names such as Idle are model-specific.
  std::string name;
  /// Number of motions in the group; valid indices are in [0, count).
  int count = 0;
};

/// @brief Read-only metadata discovered while preparing a loaded asset.
/// Collections describe model capabilities, not the current playback state; optional capabilities may be absent.
struct ModelInfo {
  /// Positive model-declared canvas dimensions in pixels, also used as the texture's intrinsic logical size.
  Size canvas;
  /// Available groups and their motion counts, used with ModelController::PlayMotion().
  std::vector<MotionGroup> motion_groups;
  /// Exact expression names accepted by ModelController::SetExpression(); may be empty.
  std::vector<std::string> expressions;
  /// Parameter IDs, defaults, and bounds available for ParameterInput overrides.
  std::vector<ParameterInfo> parameters;
  /// Model-defined area names that may be delivered by ModelEvents::HitAreaTapped.
  std::vector<std::string> hit_areas;
};

/// @brief Shared handle to an immutable loaded model package and its metadata.
/// An asset retains dependency data and runtime resources needed to create players. It holds no per-instance
/// animation state. Multiple ModelView instances can use one asset with independent playback and controllers.
/// A successful LoadModelAsync() produces a valid asset; graphics readiness is reported separately by the view.
class ModelAsset {
public:
  ModelAsset() = default;

  /// @brief Tests whether this handle contains a successfully loaded asset.
  /// @return True when Info() and ModelView() can use the asset; false for an empty handle.
  /// This does not indicate whether any mounted player has published a frame.
  [[nodiscard]] bool HasValue() const noexcept;

  /// @brief Accesses the loaded model's immutable capability metadata.
  /// @return A reference valid while at least one handle or player retains the same asset data.
  /// @throws std::logic_error If HasValue() is false.
  /// @see ModelInfo
  [[nodiscard]] const ModelInfo& Info() const;
  bool operator==(const ModelAsset&) const = default;

private:
  std::shared_ptr<const detail::AssetData> data_;
  friend struct detail::Access;
};

/// @brief Positive resource bounds applied by LoadModelAsync().
/// These bound individual loading checks, not the application's total memory or GPU usage. Cubism Native image-validation
/// failures currently report InvalidModel even when caused by a texture dimension or decoded-byte bound.
struct LoadLimits {
  /// Maximum encoded bytes read for one dependency, including the manifest; defaults to 64 MiB.
  std::size_t file_bytes = 64 * 1024 * 1024;
  /// Maximum total encoded package bytes; also used as a separate Cubism Native decoded-image budget. Defaults to 256 MiB.
  std::size_t total_bytes = 256 * 1024 * 1024;
  /// Maximum number of distinct files read, including the manifest; defaults to 1,024.
  std::size_t files = 1024;
  /// Maximum width or height in pixels for a decoded model texture; defaults to 8,192.
  int texture_dimension = 8192;
};

/// @brief Application-provided asynchronous access to a raw model package.
/// The callback receives a normalized path relative to the raw resource root of the entry's domain, without
/// the raw/ prefix. For an entry app:raw/Character/Character.model3.json, it opens Character/Character.model3.json
/// first; a manifest reference textures/main.png is requested as Character/textures/main.png.
/// Paths preserve case and use forward slashes. The callback must resolve them in the entry's resource domain;
/// the loader does not perform resource lookup or call UseRawResource().
///
/// Return a Task yielding IoResult<AsyncInputStream>: success owns an independent stream positioned at byte zero;
/// failure carries the storage error. Keep backing resources alive through that stream. Convert recoverable opening
/// exceptions to IoResult in the adapter; exceptions thrown by the callback otherwise propagate from loading.
/// The callback runs in the loading task's environment, not on an arbitrary library-selected worker thread.
/// @see LoadModelAsync
using OpenResourceAsync = std::function<Task<IoResult<AsyncInputStream>>(std::string relative_path)>;

/// @brief Loads and validates a Cubism model3 package from the local file system.
/// @param entry_file Path to the .model3.json manifest. Its parent directory is the package root;
/// supported declared dependencies are resolved relative to that directory and opened with File::OpenReadAsync().
/// No application-provided resource callback is needed. On Web, File addresses the virtual file system.
/// @param limits Positive byte, file-count, and texture-dimension bounds; omitted values use LoadLimits defaults.
/// @return A task yielding an immutable ModelAsset on success, or LoadError with available path and I/O context.
/// Dependency paths must stay lexically within the package root; symbolic links follow File semantics.
/// File-opening errors retain their original IoError in LoadError::cause. Dependency error paths are package-relative.
/// Successful loading does not create a ready mounted renderer; wait for ModelEvents::Ready before issuing commands.
/// @throws std::invalid_argument When any loading bound is zero or negative as applicable.
/// Unrecoverable runtime exceptions may propagate when the task executes.
///
/// Launch from the application's TaskScope rather than performing I/O during composition. Cancellation follows the
/// owning task scope and suppresses result delivery; an already-running Cubism Native decode may finish before cleanup.
/// Textures for Cubism Native are decoded as PNG during asset preparation and retained for later GPU upload.
/// @code{.cpp}
/// Task<void> LoadCharacter(File entry, State<ModelAsset> asset, State<std::string> status) {
///   auto result = co_await LoadModelAsync(std::move(entry));
///   if (!result.Succeeded()) {
///     status = result.Error().path + ": " + result.Error().message;
///     co_return;
///   }
///   asset = std::move(result).Value();
///   status = "Preparing graphics...";
/// }
/// @endcode
/// @see LoadLimits, ModelView
Task<Result<ModelAsset, LoadError>> LoadModelAsync(File entry_file, LoadLimits limits = {});

/// @brief Loads and validates a Cubism model3 package through application-owned raw-resource access.
/// @param entry_resource Generated raw identifier for the .model3.json manifest, such as
/// app::raw::Character_Character_model3_json. Its key must start with raw/ and identify a file.
/// @param open_resource Nonempty callback opening the manifest and each supported declared dependency within
/// entry_resource.Domain(). Paths are relative to that domain's raw root, with no raw/ prefix.
/// Captured resource access is retained while the loading task needs it.
/// @param limits Positive byte, file-count, and texture-dimension bounds; omitted values use LoadLimits defaults.
/// @return A task yielding an immutable ModelAsset on success, or LoadError with available path and I/O context.
/// Absolute dependency paths, backslashes, colons, NUL characters, and traversal outside the raw root are rejected.
/// Successful loading does not create a ready renderer; wait for ModelEvents::Ready before issuing commands.
/// @throws std::invalid_argument When the callback is empty or any loading bound is zero or negative as applicable.
/// Callback exceptions and unrecoverable runtime exceptions may propagate when the task executes.
///
/// Resolve resource handles during composition and capture them in the callback; do not call UseRawResource()
/// from the callback. Task ownership, cancellation, and Cubism Native texture preparation match the File overload.
/// @code{.cpp}
/// // Inside a loading task; include the application's generated app_resources.h.
/// auto result = co_await LoadModelAsync(app::raw::Character_Character_model3_json, open_resource);
/// @endcode
/// @see OpenResourceAsync, LoadLimits, ModelView
Task<Result<ModelAsset, LoadError>> LoadModelAsync(
    RawResource entry_resource, OpenResourceAsync open_resource, LoadLimits limits = {});

/// @brief Opaque identity of an accepted motion playback, scoped to one ModelController identity.
/// Accepted playbacks receive distinct IDs across asset replacement and remounting of the same controller.
/// IDs from separate controllers are not globally unique.
struct PlaybackId {
  /// Correlation value used by MotionFinished; zero is not issued for an accepted playback.
  std::uint64_t value = 0;
  bool operator==(const PlaybackId&) const = default;
};

/// @brief Options for a single ModelController::PlayMotion() request.
struct MotionOptions {
  /// Positive Cubism motion priority, defaulting to 1. Without force, reserved/current priority can reject a request.
  int priority = 1;
  /// Bypass Cubism's priority reservation check when true; model and index validation still apply. Defaults to false.
  bool force = false;
  /// Repeat the motion when true, without a completion event for each cycle. Defaults to false.
  bool loop = false;
};

/// @brief Why an accepted motion playback ended.
enum class MotionEndReason {
  /// The non-looping playback reached its natural end.
  Completed,
  /// Another accepted motion replaced this playback.
  Interrupted,
  /// ModelController::StopMotion() explicitly stopped this playback.
  Stopped
};

/// @brief Payload delivered by ModelEvents::MotionFinished for an ended motion.
/// Delivery is not guaranteed when the view is unmounted or its binding is replaced before pending events are emitted.
struct MotionFinished {
  /// Identity returned when the ended motion was accepted by PlayMotion().
  PlaybackId playback;
  /// End classification; defaults to Completed.
  MotionEndReason reason = MotionEndReason::Completed;
};

/// @brief Stable command handle for one mounted ModelView instance.
/// Retain the handle across recompositions, for example with UseState(). It weakly attaches to a binding and does not
/// keep an unmounted player alive. Simultaneous attachment to multiple views is a configuration error.
///
/// Issue commands on the mounted UI thread, normally in Ready or input callbacks. An unattached handle returns
/// NotAttached; a binding before first publication returns NotReady. A failed binding returns its stored error.
/// Commands are not queued for future attachment. Synchronous rejection does not also emit PlaybackFailed.
class ModelController {
public:
  ModelController();

  /// @brief Requests a model motion and returns its playback identity when accepted.
  /// @param group Exact motion-group name from ModelInfo::motion_groups; consumed before the call returns.
  /// @param index Zero-based index within that group, in [0, MotionGroup::count).
  /// @param options Positive priority, optional forced replacement, and optional looping for this request.
  /// @return The accepted PlaybackId, or a binding error, UnknownMotion, PriorityRejected, or a backend error.
  /// Success means the command was accepted, not that the animation has finished or a new frame is already visible.
  /// @throws std::invalid_argument If options.priority is not positive.
  /// @throws std::logic_error If called from a thread other than the mounted UI thread.
  /// @throws std::overflow_error If this controller's playback identity space is exhausted.
  /// A replaced motion reports Interrupted through ModelEvents::MotionFinished while its binding remains live.
  /// Paused views accept requests without advancing model time until resumed.
  /// @code{.cpp}
  /// void PlayFirstMotion(const ModelAsset& asset, const ModelController& controller, State<std::string> status) {
  ///   for (const auto& group : asset.Info().motion_groups) {
  ///     if (group.count == 0) continue;
  ///     auto result = controller.PlayMotion(group.name, 0, {.priority = 2, .force = true});
  ///     status = result.Succeeded() ? "Playing " + std::to_string(result.Value().value) : result.Error().message;
  ///     return;
  ///   }
  ///   status = "This model has no motions";
  /// }
  /// @endcode
  /// @see MotionOptions, ModelEvents::Ready, ModelEvents::MotionFinished
  Result<PlaybackId, PlaybackError> PlayMotion(
      std::string_view group, int index, MotionOptions options = {}) const;

  /// @brief Stops motion playback for the attached instance.
  /// @return PlaybackError with code == PlaybackErrorCode::None on success, including when no motion is active;
  /// otherwise a binding/backend error.
  /// @throws std::logic_error If called from a thread other than the mounted UI thread.
  /// An active playback reports Stopped while the binding remains live. This does not clear expressions or change
  /// ModelOptions::paused; automatic effects may still advance the model.
  PlaybackError StopMotion() const;

  /// @brief Starts a named expression using the model's expression blending behavior.
  /// @param name Exact name from ModelInfo::expressions; consumed before the call returns.
  /// @return PlaybackError with code == PlaybackErrorCode::None on acceptance; otherwise a binding error,
  /// UnknownExpression, or a backend error.
  /// @throws std::logic_error If called from a thread other than the mounted UI thread.
  /// Expression blending may take multiple frames. This does not return a motion PlaybackId or emit MotionFinished.
  /// @code{.cpp}
  /// void ApplyFirstExpression(const ModelAsset& asset, const ModelController& controller, State<std::string> status) {
  ///   if (asset.Info().expressions.empty()) {
  ///     status = "This model has no expressions";
  ///     return;
  ///   }
  ///   const auto error = controller.SetExpression(asset.Info().expressions.front());
  ///   status = error.code == PlaybackErrorCode::None ? "Expression accepted" : error.message;
  /// }
  /// @endcode
  /// @see ClearExpression
  PlaybackError SetExpression(std::string_view name) const;

  /// @brief Stops active expression playback so subsequent evaluation uses the remaining model effects.
  /// @return PlaybackError with code == PlaybackErrorCode::None on success, including when no expression is active;
  /// otherwise a binding/backend error.
  /// @throws std::logic_error If called from a thread other than the mounted UI thread.
  /// This does not stop motion playback or remove ModelOptions::parameters and emits no MotionFinished event.
  PlaybackError ClearExpression() const;

  bool operator==(const ModelController&) const = default;

private:
  std::shared_ptr<detail::ControllerData> data_;
  friend struct detail::Access;
};

/// @brief How an external parameter value combines with the normal model evaluation.
enum class ParameterBlend {
  /// Interpolate from the evaluated value: current * (1 - weight) + value * weight.
  Override,
  /// Add a weighted offset: current + value * weight.
  Add
};

/// @brief External input applied after normal motion, expression, physics, and pose processing.
/// The final value is clamped to the model's declared range. Overrides do not persist into the next base evaluation;
/// removing an input restores that base result, and paused Add inputs do not accumulate each frame.
/// @code{.cpp}
/// View MouthPreview(ModelAsset asset, float opening) {
///   return ModelView(asset, {.parameters = {
///     {"ParamMouthOpenY", opening, ParameterBlend::Override, 1.0F},
///   }});
/// }
/// @endcode
/// This example requires a model exposing ParamMouthOpenY and a finite opening value. Discover supported IDs through
/// ModelInfo::parameters rather than assuming all models have this parameter. Overrides are not promised as
/// physics inputs.
struct ParameterInput {
  /// Exact existing parameter ID; each ID may appear only once in ModelOptions::parameters.
  std::string id;
  /// Finite parameter value or additive offset in the parameter's own units; defaults to zero.
  float value = 0.0F;
  /// Combination rule; defaults to Override.
  ParameterBlend blend = ParameterBlend::Override;
  /// Finite contribution weight in [0, 1]; defaults to full influence (1).
  float weight = 1.0F;
  bool operator==(const ParameterInput&) const = default;
};

/// @brief Persistent declarative settings for a ModelView.
/// Update these values through composition state. Invalid rate or parameter inputs throw during ModelView creation.
/// The Runtime's reduced-motion setting currently freezes time advancement even for requested motions; dirty
/// configuration and geometry can still cause a render.
struct ModelOptions {
  /// Optional stable command identity. Omission uses an internal controller; changing an explicit identity rebuilds
  /// the mounted binding. Do not attach the same controller to multiple views at once.
  std::optional<ModelController> controller;
  /// Centered image fitting within the view's bounds; defaults to Contain, which preserves the model aspect ratio.
  ImageFit fit = ImageFit::Contain;
  /// Freeze model time when true, including motions, physics, blinking, and breathing. Defaults to false.
  /// Initial publication and dirty parameter/geometry updates can still render while paused.
  bool paused = false;
  /// Finite time multiplier in [0, 8], defaulting to 1. Zero freezes time; resuming does not catch up inactive time.
  /// Active frame time is capped at 0.1 seconds before applying this multiplier to avoid catching up long stalls.
  float playback_rate = 1.0F;
  /// Enable the model's automatic eye-blink effect when available; defaults to true.
  bool auto_blink = true;
  /// Enable the automatic ParamBreath effect when that parameter exists; defaults to true.
  bool auto_breath = true;
  /// Unique, valid parameter overrides applied after base evaluation; empty by default. Removing an entry removes
  /// that override. Unknown IDs, duplicate IDs, non-finite values, or weights outside [0, 1] are configuration errors.
  std::vector<ParameterInput> parameters;
};

/// @brief Typed event keys registered through ModelView(...).On<Key>(handler).
/// Callbacks run in the mounted UI environment after internal frame/input work and may issue controller commands.
/// No completion or failure delivery is guaranteed after unmounting or replacing the binding.
/// @see ModelView
struct ModelEvents {
  /// @brief Emitted without arguments after the binding's first successful frame publication.
  /// The controller is ready for commands. Resizing alone does not re-emit this event; a new binding can emit it again.
  /// Asset-loading success alone does not trigger Ready, and zero-sized views cannot publish their first frame.
  struct Ready : Event<void()> {};

  /// @brief Emitted with a live2d::MotionFinished payload after an accepted motion ends.
  /// The handler receives the playback ID and reason. Loop iterations and expression changes do not emit this event.
  struct MotionFinished : Event<void(live2d::MotionFinished)> {};

  /// @brief Emitted with a std::string containing the model-defined area name for a recognized primary-pointer tap.
  /// Hit areas use model drawable bounds, not per-pixel opacity. A tap outside all areas emits no event; the library
  /// does not automatically play a motion in response. Pointer cancellation or excessive movement cancels the tap.
  struct HitAreaTapped : Event<void(std::string)> {};

  /// @brief Emitted with a non-success PlaybackError when mounted preparation, rendering, or publication fails.
  /// Reported once for the failed binding. Synchronous command rejection is returned to the caller instead.
  /// The current implementation requires remounting to retry a failed backend.
  struct PlaybackFailed : Event<void(PlaybackError)> {};
};

/// @brief Declares a view that mounts an independent player for a loaded model asset.
/// @param asset Valid loaded package, retained for the mounted player's lifetime. HasValue() must be true.
/// @param options Persistent controller, fitting, time, effects, and parameter configuration;
/// defaults to ModelOptions{}.
/// @return An ordinary View supporting HuxerUI layout/modifiers and ModelEvents handlers. Provide positive layout
/// bounds so the player can prepare and publish a frame. The view uses a canvas-sized placeholder before connection.
/// @throws std::invalid_argument If the asset is empty, playback_rate is non-finite or outside [0, 8], or parameter
/// inputs contain unknown/duplicate IDs, non-finite values, or invalid weights.
/// @throws std::logic_error If the supplied controller is already attached to another view during mounting
/// or reconciliation.
///
/// Invoke from the HuxerUI composition environment. Graphics preparation follows mounting; operational failures are
/// reported by ModelEvents::PlaybackFailed. Stable asset and controller identities preserve playback across ordinary
/// recomposition. Replacing either rebuilds the binding; unmounting cancels preparation and detaches the controller.
/// No root-level installation is required, and loading the asset remains the application's responsibility.
/// @code{.cpp}
/// [[huxerui::composable]]
/// View CharacterPreview(ModelAsset asset) {
///   const ModelController controller = UseState(ModelController{});
///   auto paused = UseState(false);
///   auto status = UseState(std::string("Preparing graphics..."));
///   return Column {
///     ModelView(asset, {.controller = controller, .paused = paused})
///         .On<ModelEvents::Ready>([status] { status = "Ready"; })
///         .On<ModelEvents::HitAreaTapped>([status](std::string name) { status = "Tapped: " + name; })
///         .On<ModelEvents::MotionFinished>([status](MotionFinished event) {
///           status = "Motion ended: " + std::to_string(event.playback.value);
///         })
///         .On<ModelEvents::PlaybackFailed>([status](PlaybackError error) { status = error.message; })
///         .With(Frame{.width = 320.0F, .height = 480.0F}),
///     Text(status),
///     Button(paused ? "Resume" : "Pause").OnClick([paused] { paused = !paused; }),
///   }.With(Spacing(8.0F));
/// }
/// @endcode
/// @see LoadModelAsync, ModelAsset, ModelController, ModelOptions, ModelEvents
[[nodiscard]] View ModelView(ModelAsset asset, ModelOptions options = {});

} // namespace huxerui::live2d
