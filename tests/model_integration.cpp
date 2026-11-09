#include <huxerui/huxerui.h>
#include <huxerui/live2d.h>
#include <huxerui/testing/ui_test.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#define LIVE2D_TEST_DISCARD(...) static_cast<void>(__VA_ARGS__)

using namespace huxerui;
using namespace huxerui::live2d;
using namespace huxerui::testing;
using namespace std::chrono_literals;

namespace {

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  State<ModelAsset> asset;
  State<bool> mounted;
  State<float> playback_rate;
  ModelController controller;
  bool loaded = false;
  std::string error;
  int ready = 0;
  std::vector<MotionFinished> finished;
};

Fixture* active = nullptr;

Task<IoResult<AsyncInputStream>> OpenManifest(RawAsset manifest) {
  co_return IoResult<AsyncInputStream>::Success(co_await manifest.OpenReadAsync());
}

Task<void> Load(Fixture* fixture) {
  const File model_file = File(LIVE2D_TEST_MODEL_ROOT).Child("Wanko.model3.json");
  auto invalid_file = co_await LoadModelAsync(File("/"));
  Require(!invalid_file.Succeeded() && invalid_file.Error().code == LoadErrorCode::InvalidPath,
          "Root directory accepted as model entry");
  auto missing = co_await LoadModelAsync(File(LIVE2D_TEST_MODEL_ROOT).Child("missing.model3.json"));
  Require(!missing.Succeeded() && missing.Error().code == LoadErrorCode::Io && missing.Error().cause &&
          missing.Error().cause->code == IoErrorCode::NotFound, "Missing file did not preserve its IO error");
  auto limited = co_await LoadModelAsync(model_file, {.file_bytes = 8});
  Require(!limited.Succeeded() && limited.Error().code == LoadErrorCode::ResourceLimit, "Byte limit not enforced");
  auto oversized = co_await LoadModelAsync(model_file, {.texture_dimension = 1});
  Require(!oversized.Succeeded() && oversized.Error().code == LoadErrorCode::InvalidModel,
          "Worker texture validation did not return a load error");

  bool rejected_empty_opener = false;
  try {
    LIVE2D_TEST_DISCARD(co_await LoadModelAsync(RawResource("test", "raw/Wanko.model3.json"), {}));
  } catch (const std::invalid_argument&) { rejected_empty_opener = true; }
  Require(rejected_empty_opener, "Empty resource opener accepted");
  bool rejected_limits = false;
  try {
    LIVE2D_TEST_DISCARD(co_await LoadModelAsync(model_file, {.files = 0}));
  } catch (const std::invalid_argument&) { rejected_limits = true; }
  Require(rejected_limits, "Invalid file limits accepted");

  const std::string unsafe_manifest =
      R"({"Version":3,"FileReferences":{"Moc":"../outside.moc3","Textures":["texture.png"]}})";
  const auto manifest = RawAsset::CopyBytes(std::as_bytes(std::span(unsafe_manifest)));
  std::vector<std::string> requests;
  const auto open_manifest = [&requests, manifest](std::string path) {
    requests.push_back(std::move(path));
    return OpenManifest(manifest);
  };
  auto invalid_resource = co_await LoadModelAsync(RawResource("test", "images/entry"), open_manifest);
  Require(!invalid_resource.Succeeded() && invalid_resource.Error().code == LoadErrorCode::InvalidPath &&
          requests.empty(), "Invalid raw key reached the opener");
  auto traversal = co_await LoadModelAsync(RawResource("test", "raw/entry.model3.json"), open_manifest);
  Require(!traversal.Succeeded() && traversal.Error().code == LoadErrorCode::InvalidPath &&
          requests == std::vector<std::string>{"entry.model3.json"}, "Raw package traversal accepted");

  struct TemporaryDirectory {
    std::filesystem::path path;
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
  } temporary{std::filesystem::temp_directory_path() /
              ("live2d-loader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  std::filesystem::create_directory(temporary.path);
  const File unsafe_file = File(temporary.path.string()).Child("entry.model3.json");
  Require(unsafe_file.WriteString(unsafe_manifest), "Cannot write traversal fixture");
  auto file_traversal = co_await LoadModelAsync(unsafe_file);
  Require(!file_traversal.Succeeded() && file_traversal.Error().code == LoadErrorCode::InvalidPath,
          "File package traversal accepted");

  requests.clear();
  const auto open_resource = [&requests](std::string path) {
    Require(path.starts_with("models/Wanko/"), "Callback path lost the raw-root-relative entry directory");
    requests.push_back(path);
    return File(LIVE2D_TEST_MODEL_ROOT).Resolve(path.substr(13)).OpenReadAsync();
  };
  auto raw_missing = co_await LoadModelAsync(RawResource("test", "raw/models/Wanko/missing.model3.json"), open_resource);
  Require(!raw_missing.Succeeded() && raw_missing.Error().cause &&
          raw_missing.Error().cause->code == IoErrorCode::NotFound, "Raw opener IO error was lost");
  requests.clear();
  const RawResource raw_entry("test", "raw/models/Wanko/Wanko.model3.json");
  auto raw_model = co_await LoadModelAsync(raw_entry, open_resource);
  Require(raw_model.Succeeded(), "Raw model failed to load");
  Require(requests.front() == "models/Wanko/Wanko.model3.json" &&
          std::find(requests.begin(), requests.end(), "models/Wanko/Wanko.1024/texture_00.png") != requests.end() &&
          std::find(requests.begin(), requests.end(), "models/Wanko/motions/idle_01.motion3.json") != requests.end(),
          "Raw dependency callback paths were incorrect");
  auto raw_limited = co_await LoadModelAsync(raw_entry, open_resource, {.file_bytes = 8});
  Require(!raw_limited.Succeeded() && raw_limited.Error().code == LoadErrorCode::ResourceLimit,
          "Raw byte limit not enforced");
  auto result = co_await LoadModelAsync(model_file);
  Require(result.Succeeded() && result.Value().Info().parameters.size() == raw_model.Value().Info().parameters.size(),
          "File and raw loaders produced different metadata");
  fixture->loaded = true;
  if (!result.Succeeded()) { fixture->error = result.Error().message; co_return; }
  fixture->asset = std::move(result).Value();
}

View App() {
  auto* fixture = active;
  fixture->asset = UseState(ModelAsset{});
  fixture->mounted = UseState(true);
  fixture->playback_rate = UseState(1.0F);
  auto tasks = UseTaskScope();
  Lifecycle([tasks, fixture] { tasks.Launch(Load(fixture)); });
  if (!fixture->asset->HasValue() || !fixture->mounted) return Text("Waiting");
  return Column {
    ModelView(fixture->asset, {.controller = fixture->controller, .playback_rate = fixture->playback_rate,
                               .auto_blink = false, .auto_breath = false})
        .On<ModelEvents::Ready>([fixture] { ++fixture->ready; })
        .On<ModelEvents::PlaybackFailed>([fixture](PlaybackError error) { fixture->error = error.message; })
        .On<ModelEvents::MotionFinished>([fixture](MotionFinished event) { fixture->finished.push_back(event); }),
  };
}

} // namespace

int main() {
  try {
    Fixture fixture;
    active = &fixture;
    Require(fixture.controller.StopMotion().code == PlaybackErrorCode::NotAttached, "Unattached controller accepted Stop");
    const Application application{App};
    UiTest ui(application);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!fixture.ready && fixture.error.empty() && std::chrono::steady_clock::now() < deadline) {
      ui.Pump(16ms);
      std::this_thread::sleep_for(1ms);
    }
    if (!fixture.error.empty()) throw std::runtime_error(fixture.error);
    Require(fixture.loaded && fixture.ready == 1, "Real model did not publish its first frame");
    ui.Pump(16ms);
    Require(ui.Find(UiSelector::Type<Image>()).Count() == 1, "Published texture was not mounted");
    Require(!fixture.asset->Info().parameters.empty(), "Core metadata is missing");
    auto unknown = fixture.controller.PlayMotion("unknown", 0);
    Require(!unknown.Succeeded() && unknown.Error().code == PlaybackErrorCode::UnknownMotion, "Unknown motion accepted");
    auto first = fixture.controller.PlayMotion("Idle", 0, {.loop = true});
    Require(first.Succeeded(), "Idle motion rejected");
    auto denied = fixture.controller.PlayMotion("TapBody", 0);
    Require(!denied.Succeeded() && denied.Error().code == PlaybackErrorCode::PriorityRejected, "Priority was ignored");
    auto second = fixture.controller.PlayMotion("TapBody", 0, {.priority = 2, .force = true});
    Require(second.Succeeded() && second.Value() != first.Value(), "Playback identity was reused");
    ui.Pump(16ms);
    Require(fixture.finished.size() == 1 && fixture.finished[0].playback == first.Value() &&
            fixture.finished[0].reason == MotionEndReason::Interrupted, "Interruption event mismatch");
    Require(fixture.controller.StopMotion().code == PlaybackErrorCode::None, "Stop failed");
    ui.Pump(16ms);
    Require(fixture.finished.size() == 2 && fixture.finished[1].reason == MotionEndReason::Stopped, "Stop event missing");
    fixture.playback_rate = 8.0F;
    ui.Pump();
    auto fast = fixture.controller.PlayMotion("TapBody", 0, {.force = true});
    Require(fast.Succeeded(), "Fast motion rejected");
    for (int frame = 0; frame < 19; ++frame) ui.Pump(std::chrono::duration<double>(1.0 / 60.0));
    Require(std::any_of(fixture.finished.begin(), fixture.finished.end(), [&](const MotionFinished& event) {
      return event.playback == fast.Value() && event.reason == MotionEndReason::Completed;
    }), "Eightfold playback was truncated by the frame-time cap");
    fixture.playback_rate = 1.0F;
    fixture.mounted = false;
    ui.Pump();
    Require(fixture.controller.StopMotion().code == PlaybackErrorCode::NotAttached, "Unmount left controller attached");
    fixture.mounted = true;
    ui.Pump();
    fixture.mounted = false;
    ui.Pump();
    Require(fixture.controller.StopMotion().code == PlaybackErrorCode::NotAttached,
            "Canceled preparation left controller attached");
    const auto previous_ready = fixture.ready;
    fixture.mounted = true;
    const auto remount_deadline = std::chrono::steady_clock::now() + 15s;
    while (fixture.ready == previous_ready && fixture.error.empty() &&
           std::chrono::steady_clock::now() < remount_deadline) {
      ui.Pump(16ms);
      std::this_thread::sleep_for(1ms);
    }
    Require(fixture.error.empty() && fixture.ready == previous_ready + 1, "Asset remount did not become ready");
    std::cout << "Native Core, Metal publication, model loading and controller integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
