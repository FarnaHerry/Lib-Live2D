#pragma once

#include "model_internal.h"

#include <CubismModelSettingJson.hpp>
#include <Model/CubismUserModel.hpp>
#include <Motion/CubismMotion.hpp>
#include <mutex>

namespace huxerui::live2d::detail {

namespace cubism = Live2D::Cubism::Framework;

std::recursive_mutex& CubismMutex();
std::shared_ptr<void> AcquireCubism();
std::shared_ptr<void>& CubismGraphicsContext(const std::shared_ptr<void>& runtime);

class NativeModel : public cubism::CubismUserModel {
public:
  explicit NativeModel(const AssetData& asset);
  ~NativeModel() override;
  void Advance(double delta, const ModelOptions& options);
  PlaybackError Play(PlaybackId id, std::string_view group, int index, MotionOptions options);
  PlaybackError Stop();
  PlaybackError Expression(std::string_view name);
  PlaybackError Clear();
  std::vector<MotionFinished> TakeFinished();
  std::string Hit(Point normalized) const;
  bool Animating() const;
  cubism::CubismMatrix44 Projection() const;

private:
  const AssetData& asset_;
  std::unique_ptr<cubism::CubismModelSettingJson> setting_;
  std::optional<PlaybackId> playing_;
  cubism::CubismMotionQueueEntryHandle motion_handle_{};
  std::vector<MotionFinished> finished_;
  std::vector<float> evaluated_parameters_;
  bool evaluated_ = false;
};

class PlatformSurface {
public:
  virtual ~PlatformSurface() = default;
  virtual std::shared_ptr<ExternalTexture> Texture() const = 0;
  virtual PlaybackError Draw(NativeModel& model, Size pixels) = 0;
};

#if defined(__ANDROID__)
Task<void> PrepareGlContextAsync(std::shared_ptr<void> runtime);
#endif

std::unique_ptr<PlatformSurface> CreatePlatformSurface(const AssetData& asset, NativeModel& model);

} // namespace huxerui::live2d::detail
