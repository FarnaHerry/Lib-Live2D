#include "native_model.h"

#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Effect/CubismPose.hpp>
#include <Id/CubismIdManager.hpp>
#include <Physics/CubismPhysics.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace huxerui::live2d::detail {

NativeModel::NativeModel(const AssetData& asset) : asset_(asset) {
  const auto& entry = asset.files.at(asset.entry);
  setting_ = std::make_unique<cubism::CubismModelSettingJson>(
      reinterpret_cast<const cubism::csmByte*>(entry.data()), entry.size());
  const auto& moc = asset.files.at(asset.manifest.moc);
  LoadModel(reinterpret_cast<const cubism::csmByte*>(moc.data()), moc.size(), true);
  if (!_model) throw std::runtime_error("Cubism rejected the moc data");
  cubism::csmMap<cubism::csmString, float> layout;
  setting_->GetLayoutMap(layout);
  _modelMatrix->SetupFromLayout(layout);
  const auto slash = asset.entry.rfind('/');
  const auto base = slash == std::string::npos ? "" : asset.entry.substr(0, slash);
  auto load = [&](const char* file, auto reader) {
    if (!file || !*file) return;
    auto path = ResolvePath(base, file);
    if (!path.Succeeded()) throw std::runtime_error("Invalid model dependency path");
    const auto& data = asset.files.at(path.Value());
    reader(reinterpret_cast<const cubism::csmByte*>(data.data()), data.size());
  };
  load(setting_->GetPhysicsFileName(), [this](auto bytes, auto size) { LoadPhysics(bytes, size); });
  load(setting_->GetPoseFileName(), [this](auto bytes, auto size) { LoadPose(bytes, size); });
  if (setting_->GetEyeBlinkParameterCount() > 0) _eyeBlink = cubism::CubismEyeBlink::Create(setting_.get());
  for (int i = 0; i < _model->GetParameterCount(); ++i) {
    if (_model->GetParameterId(i) == cubism::CubismFramework::GetIdManager()->GetId("ParamBreath")) {
      _breath = cubism::CubismBreath::Create();
      cubism::csmVector<cubism::CubismBreath::BreathParameterData> parameters;
      parameters.PushBack({_model->GetParameterId(i), 0.5F, 0.5F, 3.2345F, 1.0F});
      _breath->SetParameters(parameters);
      break;
    }
  }
  _model->SaveParameters();
}

NativeModel::~NativeModel() = default;

void NativeModel::Advance(double delta, const ModelOptions& options) {
  if (delta > 0.0 || !evaluated_) {
    const float elapsed = static_cast<float>(delta);
    _model->LoadParameters();
    const bool motion_updated = _motionManager->UpdateMotion(_model, elapsed);
    _model->SaveParameters();
    if (!motion_updated && options.auto_blink && _eyeBlink) _eyeBlink->UpdateParameters(_model, elapsed);
    _expressionManager->UpdateMotion(_model, elapsed);
    if (options.auto_breath && _breath) _breath->UpdateParameters(_model, elapsed);
    if (_physics && elapsed > 0) _physics->Evaluate(_model, elapsed);
    if (_pose) _pose->UpdateParameters(_model, elapsed);
    evaluated_parameters_.resize(_model->GetParameterCount());
    for (int i = 0; i < _model->GetParameterCount(); ++i) evaluated_parameters_[i] = _model->GetParameterValue(i);
    evaluated_ = true;
    if (playing_ && _motionManager->IsFinished(motion_handle_)) {
      finished_.push_back({*playing_, MotionEndReason::Completed});
      playing_.reset();
    }
  }
  for (int i = 0; i < _model->GetParameterCount(); ++i) _model->SetParameterValue(i, evaluated_parameters_[i]);
  for (const auto& parameter : options.parameters) {
    const auto id = cubism::CubismFramework::GetIdManager()->GetId(parameter.id.c_str());
    const int index = _model->GetParameterIndex(id);
    const float current = _model->GetParameterValue(index);
    const float value = parameter.blend == ParameterBlend::Add
                            ? current + parameter.value * parameter.weight
                            : current * (1.0F - parameter.weight) + parameter.value * parameter.weight;
    _model->SetParameterValue(index, std::clamp(value, _model->GetParameterMinimumValue(index),
                                               _model->GetParameterMaximumValue(index)));
  }
  _model->Update();
}

PlaybackError NativeModel::Play(PlaybackId id, std::string_view group, int index, MotionOptions options) {
  const auto found = std::find_if(asset_.manifest.motions.begin(), asset_.manifest.motions.end(),
      [&](const MotionFile& file) { return file.group == group && file.index == index; });
  if (found == asset_.manifest.motions.end()) return {PlaybackErrorCode::UnknownMotion, "Unknown model motion"};
  if (!options.force && !_motionManager->ReserveMotion(options.priority)) {
    return {PlaybackErrorCode::PriorityRejected, "Motion priority was rejected"};
  }
  const auto& bytes = asset_.files.at(found->path);
  auto* motion = static_cast<cubism::CubismMotion*>(LoadMotion(
      reinterpret_cast<const cubism::csmByte*>(bytes.data()), bytes.size(), found->path.c_str(),
      nullptr, nullptr, setting_.get(), found->group.c_str(), index, true));
  if (!motion) {
    _motionManager->SetReservePriority(0);
    return {PlaybackErrorCode::BackendFailed, "Cubism rejected the motion data"};
  }
  motion->SetLoop(options.loop);
  cubism::csmVector<cubism::CubismIdHandle> blink_ids;
  cubism::csmVector<cubism::CubismIdHandle> lip_ids;
  for (int i = 0; i < setting_->GetEyeBlinkParameterCount(); ++i) blink_ids.PushBack(setting_->GetEyeBlinkParameterId(i));
  for (int i = 0; i < setting_->GetLipSyncParameterCount(); ++i) lip_ids.PushBack(setting_->GetLipSyncParameterId(i));
  motion->SetEffectIds(blink_ids, lip_ids);
  if (playing_) finished_.push_back({*playing_, MotionEndReason::Interrupted});
  motion_handle_ = _motionManager->StartMotionPriority(motion, true, options.priority);
  playing_ = id;
  evaluated_ = false;
  return {};
}

PlaybackError NativeModel::Stop() {
  _motionManager->StopAllMotions();
  if (playing_) finished_.push_back({*playing_, MotionEndReason::Stopped});
  playing_.reset();
  return {};
}

PlaybackError NativeModel::Expression(std::string_view name) {
  const auto found = asset_.manifest.expressions.find(std::string(name));
  if (found == asset_.manifest.expressions.end()) return {PlaybackErrorCode::UnknownExpression, "Unknown expression"};
  const auto& bytes = asset_.files.at(found->second);
  auto* expression = LoadExpression(reinterpret_cast<const cubism::csmByte*>(bytes.data()), bytes.size(), found->first.c_str());
  if (!expression) return {PlaybackErrorCode::BackendFailed, "Cubism rejected the expression data"};
  _expressionManager->StartMotion(expression, true);
  evaluated_ = false;
  return {};
}

PlaybackError NativeModel::Clear() {
  _expressionManager->StopAllMotions();
  evaluated_ = false;
  return {};
}

std::vector<MotionFinished> NativeModel::TakeFinished() {
  auto events = std::move(finished_);
  finished_.clear();
  return events;
}

bool NativeModel::Animating() const {
  return !_motionManager->IsFinished() || !_expressionManager->IsFinished();
}

cubism::CubismMatrix44 NativeModel::Projection() const {
  cubism::CubismMatrix44 projection;
  projection.Scale(_model->GetCanvasHeight() / _model->GetCanvasWidth(), 1.0F);
  projection.MultiplyByMatrix(_modelMatrix);
  return projection;
}

std::string NativeModel::Hit(Point normalized) const {
  const float x = (normalized.x * 2.0F - 1.0F) * _model->GetCanvasWidth() / _model->GetCanvasHeight();
  const float y = 1.0F - normalized.y * 2.0F;
  for (int i = 0; i < setting_->GetHitAreasCount(); ++i) {
    const int index = _model->GetDrawableIndex(setting_->GetHitAreaId(i));
    if (index < 0 || _model->GetDrawableVertexCount(index) == 0) continue;
    const auto* vertices = _model->GetDrawableVertices(index);
    float left = vertices[0], right = left, top = vertices[1], bottom = top;
    for (int j = 1; j < _model->GetDrawableVertexCount(index); ++j) {
      left = std::min(left, vertices[j * 2]); right = std::max(right, vertices[j * 2]);
      top = std::min(top, vertices[j * 2 + 1]); bottom = std::max(bottom, vertices[j * 2 + 1]);
    }
    const float local_x = _modelMatrix->InvertTransformX(x), local_y = _modelMatrix->InvertTransformY(y);
    if (local_x >= left && local_x <= right && local_y >= top && local_y <= bottom) return setting_->GetHitAreaName(i);
  }
  return {};
}

namespace {

class NativePlayer final : public Player {
public:
  explicit NativePlayer(std::shared_ptr<const AssetData> asset) : asset_(std::move(asset)) {
    model_ = std::make_unique<NativeModel>(*asset_);
  }
  Task<void> PrepareAsync() {
#if defined(__ANDROID__)
    co_await PrepareGlContextAsync(asset_->runtime);
#endif
    std::lock_guard lock(CubismMutex());
    surface_ = CreatePlatformSurface(*asset_, *model_);
    co_return;
  }
  ~NativePlayer() override {
    std::lock_guard lock(CubismMutex());
    surface_.reset();
    model_.reset();
  }
  std::shared_ptr<ExternalTexture> Texture() const override { return surface_->Texture(); }
  PlaybackError Render(Size pixels, double delta, const ModelOptions& options) override {
    std::lock_guard lock(CubismMutex());
    model_->Advance(delta, options);
    return surface_->Draw(*model_, pixels);
  }
  PlaybackError PlayMotion(PlaybackId id, std::string_view group, int index, MotionOptions options) override {
    std::lock_guard lock(CubismMutex()); return model_->Play(id, group, index, options);
  }
  PlaybackError StopMotion() override { std::lock_guard lock(CubismMutex()); return model_->Stop(); }
  PlaybackError SetExpression(std::string_view name) override {
    std::lock_guard lock(CubismMutex()); return model_->Expression(name);
  }
  PlaybackError ClearExpression() override { std::lock_guard lock(CubismMutex()); return model_->Clear(); }
  std::vector<MotionFinished> TakeFinished() override { return model_->TakeFinished(); }
  std::string Hit(Point point) const override { std::lock_guard lock(CubismMutex()); return model_->Hit(point); }
  bool IsAnimating() const override { return model_->Animating(); }
private:
  std::shared_ptr<const AssetData> asset_;
  std::unique_ptr<NativeModel> model_;
  std::unique_ptr<PlatformSurface> surface_;
};

} // namespace

Task<Result<std::shared_ptr<Player>, PlaybackError>> CreatePlayerAsync(std::shared_ptr<const AssetData> asset) {
  try {
    std::shared_ptr<NativePlayer> player;
    {
      std::lock_guard lock(CubismMutex());
      player = std::make_shared<NativePlayer>(std::move(asset));
    }
    co_await player->PrepareAsync();
    co_return Result<std::shared_ptr<Player>, PlaybackError>::Success(std::move(player));
  } catch (const std::runtime_error& error) {
    co_return Result<std::shared_ptr<Player>, PlaybackError>::Failure({PlaybackErrorCode::BackendFailed, error.what()});
  }
}

} // namespace huxerui::live2d::detail
