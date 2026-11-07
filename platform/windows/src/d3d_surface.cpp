#include "native/native_model.h"
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <huxerui/windows/external_texture.h>
#include <wrl/client.h>
#include <stdexcept>

namespace huxerui::live2d::detail {
namespace {

using Microsoft::WRL::ComPtr;
using Renderer = cubism::Rendering::CubismRenderer_D3D11;

void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) throw std::runtime_error(operation);
}

struct Device {
  Device() {
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "Cannot create a D3D11 device");
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
};

std::shared_ptr<Device> AcquireDevice() {
  static std::weak_ptr<Device> shared;
  auto device = shared.lock();
  if (!device) { device = std::make_shared<Device>(); shared = device; }
  return device;
}

class D3dSurface final : public PlatformSurface {
public:
  D3dSurface(const AssetData& asset, NativeModel& model)
      : device_(AcquireDevice()), model_(model), mailbox_(std::make_shared<windows::D3D11Texture>(asset.info.canvas)) {
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures;
    for (const auto& image : asset.images) {
      D3D11_TEXTURE2D_DESC descriptor{};
      descriptor.Width = image.width;
      descriptor.Height = image.height;
      descriptor.MipLevels = descriptor.ArraySize = 1;
      descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      descriptor.SampleDesc.Count = 1;
      descriptor.Usage = D3D11_USAGE_IMMUTABLE;
      descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA data{image.rgba.data(), static_cast<UINT>(image.width * 4), 0};
      ComPtr<ID3D11Texture2D> texture;
      Check(device_->device->CreateTexture2D(&descriptor, &data, &texture), "Cannot upload a model texture to D3D11");
      ComPtr<ID3D11ShaderResourceView> view;
      Check(device_->device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Cannot create a D3D11 texture view");
      textures.push_back(std::move(view));
    }
    textures_ = std::move(textures);
    Renderer::SetConstantSettings(1, device_->device.Get());
    model_.CreateRenderer(1, 1);
    auto* shaders = cubism::Rendering::CubismDeviceInfo_D3D11::GetDeviceInfo(device_->device.Get())->GetShader();
    for (cubism::csmUint32 index = 0; index < cubism::ShaderNames_Max; ++index) {
      if (!shaders->GetVertexShader(index) || !shaders->GetPixelShader(index)) {
        model_.DeleteRenderer();
        if (device_.use_count() == 1) cubism::Rendering::CubismDeviceInfo_D3D11::ReleaseAllDeviceInfo();
        throw std::runtime_error("Cannot initialize Cubism D3D11 shaders");
      }
    }
    auto* renderer = model_.GetRenderer<Renderer>();
    renderer->IsPremultipliedAlpha(false);
    for (std::size_t index = 0; index < textures_.size(); ++index) renderer->BindTexture(index, textures_[index].Get());
  }

  ~D3dSurface() override {
    model_.DeleteRenderer();
    mailbox_->Finish();
    if (device_.use_count() == 1) cubism::Rendering::CubismDeviceInfo_D3D11::ReleaseAllDeviceInfo();
  }

  std::shared_ptr<ExternalTexture> Texture() const override { return mailbox_; }

  PlaybackError Draw(NativeModel& model, Size pixels) override {
    try {
      const UINT width = static_cast<UINT>(pixels.width), height = static_cast<UINT>(pixels.height);
      if (pixels_ != pixels) {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.MipLevels = descriptor.ArraySize = 1;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = 1;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> view;
        Check(device_->device->CreateTexture2D(&descriptor, nullptr, &target), "Cannot allocate D3D11 render target");
        Check(device_->device->CreateRenderTargetView(target.Get(), nullptr, &view), "Cannot create D3D11 target view");
        target_ = std::move(target);
        view_ = std::move(view);
        model.SetRenderTargetSize(width, height);
        pixels_ = pixels;
      }
      const float transparent[]{0, 0, 0, 0};
      auto* context = device_->context.Get();
      auto* view = view_.Get();
      context->OMSetRenderTargets(1, &view, nullptr);
      context->ClearRenderTargetView(view, transparent);
      const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
      context->RSSetViewports(1, &viewport);
      auto* renderer = model.GetRenderer<Renderer>();
      renderer->StartFrame(context);
      auto projection = model.Projection();
      renderer->SetMvpMatrix(&projection);
      renderer->DrawModel();
      renderer->EndFrame();
      Check(device_->device->GetDeviceRemovedReason(), "The D3D11 device was lost");
      mailbox_->Publish({target_.Get(), windows::D3D11Texture::Alpha::Premultiplied});
      return {};
    } catch (const std::runtime_error& error) {
      return {PlaybackErrorCode::BackendFailed, error.what()};
    }
  }

private:
  std::shared_ptr<Device> device_;
  NativeModel& model_;
  std::shared_ptr<windows::D3D11Texture> mailbox_;
  std::vector<ComPtr<ID3D11ShaderResourceView>> textures_;
  ComPtr<ID3D11Texture2D> target_;
  ComPtr<ID3D11RenderTargetView> view_;
  Size pixels_{};
};

} // namespace

std::unique_ptr<PlatformSurface> CreatePlatformSurface(const AssetData& asset, NativeModel& model) {
  return std::make_unique<D3dSurface>(asset, model);
}

} // namespace huxerui::live2d::detail
