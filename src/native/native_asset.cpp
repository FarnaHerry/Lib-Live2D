#include "native_model.h"
#include "image_decode.h"

#include <Id/CubismId.hpp>
#include <Utils/CubismJson.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

#if defined(LIVE2D_EMBEDDED_SHADERS)
#include "embedded_shaders.h"
#endif

namespace huxerui::live2d::detail {

std::recursive_mutex& CubismMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

namespace {

class Allocator final : public cubism::ICubismAllocator {
public:
  void* Allocate(cubism::csmSizeType size) override { return std::malloc(size); }
  void Deallocate(void* memory) override { std::free(memory); }
  void* AllocateAligned(cubism::csmSizeType size, cubism::csmUint32 alignment) override {
    const auto align = std::max<std::size_t>(alignment, alignof(void*));
    auto* raw = std::malloc(size + align - 1 + sizeof(void*));
    if (!raw) return nullptr;
    const auto address = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    auto* aligned = reinterpret_cast<void*>((address + align - 1) & ~(align - 1));
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return aligned;
  }
  void DeallocateAligned(void* memory) override {
    if (memory) std::free(reinterpret_cast<void**>(memory)[-1]);
  }
};

#if defined(LIVE2D_EMBEDDED_SHADERS)
cubism::csmByte* LoadShader(const std::string path, cubism::csmSizeInt* size) {
  *size = 0;
  for (const auto& shader : embedded_shaders) {
    if (shader.path != path) continue;
    auto* bytes = static_cast<cubism::csmByte*>(std::malloc(shader.source.size()));
    if (!bytes) return nullptr;
    std::memcpy(bytes, shader.source.data(), shader.source.size());
    *size = static_cast<cubism::csmSizeInt>(shader.source.size());
    return bytes;
  }
  return nullptr;
}

void ReleaseShader(cubism::csmByte* bytes) { std::free(bytes); }
#endif

class Runtime {
public:
  Runtime() {
    if (cubism::CubismFramework::IsStarted()) throw std::logic_error("Another Cubism provider is already initialized");
    options_.LoggingLevel = cubism::CubismFramework::Option::LogLevel_Off;
#if defined(LIVE2D_EMBEDDED_SHADERS)
    options_.LoadFileFunction = LoadShader;
    options_.ReleaseBytesFunction = ReleaseShader;
#endif
    if (!cubism::CubismFramework::StartUp(&allocator_, &options_)) throw std::runtime_error("Cannot start Cubism");
    cubism::CubismFramework::Initialize();
  }
  ~Runtime() {
    std::lock_guard lock(CubismMutex());
    graphics.reset();
    cubism::CubismFramework::Dispose();
    cubism::CubismFramework::CleanUp();
  }
  std::shared_ptr<void> graphics;
private:
  Allocator allocator_;
  cubism::CubismFramework::Option options_{};
};

} // namespace

std::shared_ptr<void>& CubismGraphicsContext(const std::shared_ptr<void>& runtime) {
  return std::static_pointer_cast<Runtime>(runtime)->graphics;
}

std::shared_ptr<void> AcquireCubism() {
  std::lock_guard lock(CubismMutex());
  static std::weak_ptr<Runtime> runtime;
  auto current = runtime.lock();
  if (!current) {
    current = std::make_shared<Runtime>();
    runtime = current;
  }
  return current;
}

Result<Manifest, LoadError> ParseManifest(AssetData& asset) {
  using Parsed = Result<Manifest, LoadError>;
  std::lock_guard lock(CubismMutex());
  asset.runtime = AcquireCubism();
  const auto& bytes = asset.files.at(asset.entry);
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Parsed::Failure({LoadErrorCode::InvalidModel, asset.entry, "Invalid model manifest size"});
  }
  cubism::CubismModelSettingJson setting(reinterpret_cast<const cubism::csmByte*>(bytes.data()), bytes.size());
  auto* json = setting.GetJsonPointer();
  if (!json || !json->GetRoot().IsMap() || json->GetRoot()["Version"].ToInt() != 3 ||
      !json->GetRoot()["FileReferences"].IsMap() ||
      !json->GetRoot()["FileReferences"]["Moc"].IsString() || setting.GetTextureCount() <= 0) {
    return Parsed::Failure({LoadErrorCode::InvalidModel, asset.entry, "Expected a Cubism model3 manifest"});
  }
  Manifest manifest;
  const auto slash = asset.entry.rfind('/');
  const std::string base = slash == std::string::npos ? "" : asset.entry.substr(0, slash);
  std::optional<LoadError> error;
  std::set<std::string> dependencies;
  auto resolve = [&](const char* value) {
    auto path = ResolvePath(base, value);
    if (!path.Succeeded()) { error = path.Error(); error->path = asset.entry; return std::string{}; }
    dependencies.insert(path.Value());
    return path.Value();
  };
  manifest.moc = resolve(setting.GetModelFileName());
  for (int i = 0; i < setting.GetTextureCount(); ++i) manifest.textures.push_back(resolve(setting.GetTextureFileName(i)));
  for (int group = 0; group < setting.GetMotionGroupCount(); ++group) {
    const std::string name = setting.GetMotionGroupName(group);
    for (int index = 0; index < setting.GetMotionCount(name.c_str()); ++index) {
      manifest.motions.push_back({name, index, resolve(setting.GetMotionFileName(name.c_str(), index))});
    }
  }
  for (int index = 0; index < setting.GetExpressionCount(); ++index) {
    const auto [entry, inserted] = manifest.expressions.emplace(
        setting.GetExpressionName(index), resolve(setting.GetExpressionFileName(index)));
    if (!inserted) return Parsed::Failure({LoadErrorCode::InvalidModel, asset.entry, "Duplicate expression name"});
  }
  for (const char* file : {setting.GetPhysicsFileName(), setting.GetPoseFileName(),
                          setting.GetDisplayInfoFileName(), setting.GetUserDataFile()}) {
    if (file && *file) resolve(file);
  }
  if (error) return Parsed::Failure(*error);
  manifest.dependencies.assign(dependencies.begin(), dependencies.end());
  return Parsed::Success(std::move(manifest));
}

Task<Result<ModelInfo, LoadError>> PrepareAssetAsync(AssetData& asset) {
  using Prepared = Result<ModelInfo, LoadError>;
  const auto& moc = asset.files.at(asset.manifest.moc);
  if (moc.size() < 64 || moc.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    co_return Prepared::Failure({LoadErrorCode::InvalidModel, asset.manifest.moc, "Invalid moc size"});
  }
  try {
    asset.images = co_await DecodeImagesAsync(asset);
    std::lock_guard lock(CubismMutex());
    NativeModel model(asset);
    auto* core_model = model.GetModel();
    ModelInfo info;
    info.canvas = {core_model->GetCanvasWidthPixel(), core_model->GetCanvasHeightPixel()};
    if (!(info.canvas.width > 0 && info.canvas.height > 0) || core_model->GetDrawableCount() > 100000) {
      co_return Prepared::Failure({LoadErrorCode::ResourceLimit, asset.manifest.moc, "Unsupported model geometry"});
    }
    for (int index = 0; index < core_model->GetParameterCount(); ++index) {
      info.parameters.push_back({core_model->GetParameterId(index)->GetString().GetRawString(),
          core_model->GetParameterMinimumValue(index), core_model->GetParameterMaximumValue(index),
          core_model->GetParameterDefaultValue(index)});
    }
    const auto& entry = asset.files.at(asset.entry);
    cubism::CubismModelSettingJson setting(reinterpret_cast<const cubism::csmByte*>(entry.data()), entry.size());
    for (int i = 0; i < setting.GetMotionGroupCount(); ++i) {
      const std::string name = setting.GetMotionGroupName(i);
      info.motion_groups.push_back({name, setting.GetMotionCount(name.c_str())});
    }
    for (const auto& [name, path] : asset.manifest.expressions) info.expressions.push_back(name);
    for (int i = 0; i < setting.GetHitAreasCount(); ++i) info.hit_areas.emplace_back(setting.GetHitAreaName(i));
    co_return Prepared::Success(std::move(info));
  } catch (const std::runtime_error& error) {
    co_return Prepared::Failure({LoadErrorCode::InvalidModel, asset.entry, error.what()});
  }
}

} // namespace huxerui::live2d::detail
