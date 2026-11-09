#include "native/native_model.h"

#include <huxerui/macos/external_texture.h>
#include <Rendering/Metal/CubismRenderer_Metal.hpp>
#include <stdexcept>

namespace huxerui::live2d::detail {
namespace {

using Mailbox = huxerui::macos::MetalTexture;
using Renderer = cubism::Rendering::CubismRenderer_Metal;
std::size_t surface_count = 0;

class MetalSurface final : public PlatformSurface {
public:
  MetalSurface(const AssetData& asset, NativeModel& model) : model_(model), mailbox_(std::make_shared<Mailbox>(asset.info.canvas)) {
    @autoreleasepool {
      device_ = MTLCreateSystemDefaultDevice();
      if (!device_) throw std::runtime_error("No Metal device is available");
      queue_ = [device_ newCommandQueue];
      if (!queue_) throw std::runtime_error("Cannot create a Metal command queue");
      if (![[NSBundle mainBundle] URLForResource:@"MetalShaders" withExtension:@"metallib"
                                   subdirectory:@"FrameworkMetallibs"]) {
        throw std::runtime_error("Cubism FrameworkMetallibs are missing from the application bundle");
      }
      NSMutableArray<id<MTLTexture>>* textures = [NSMutableArray array];
      for (const auto& decoded : asset.images) {
        auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:decoded.width height:decoded.height mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
        if (!texture) throw std::runtime_error("Cannot allocate model texture");
        [texture replaceRegion:MTLRegionMake2D(0, 0, decoded.width, decoded.height) mipmapLevel:0
                     withBytes:decoded.rgba.data() bytesPerRow:decoded.width * 4];
        [textures addObject:texture];
      }
      textures_ = textures;
      Renderer::SetConstantSettings(device_);
      model_.CreateRenderer(1, 1);
      auto* renderer = model_.GetRenderer<Renderer>();
      renderer->IsPremultipliedAlpha(false);
      for (NSUInteger index = 0; index < textures_.count; ++index) renderer->BindTexture(index, textures_[index]);
      ++surface_count;
    }
  }

  ~MetalSurface() override {
    model_.DeleteRenderer();
    if (--surface_count == 0) cubism::Rendering::CubismDeviceInfo_Metal::ReleaseAllDeviceInfo();
    mailbox_->Finish();
  }

  std::shared_ptr<ExternalTexture> Texture() const override { return mailbox_; }

  PlaybackError Draw(NativeModel& model, Size pixels) override {
    @autoreleasepool {
      const NSUInteger width = static_cast<NSUInteger>(pixels.width), height = static_cast<NSUInteger>(pixels.height);
      if (!target_ || target_.width != width || target_.height != height) {
        auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        target_ = [device_ newTextureWithDescriptor:descriptor];
        if (!target_) return {PlaybackErrorCode::BackendFailed, "Cannot allocate the Metal render target"};
        model.SetRenderTargetSize(width, height);
      }
      auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = target_;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      id<MTLCommandBuffer> command = [queue_ commandBuffer];
      if (!command) return {PlaybackErrorCode::BackendFailed, "Cannot create a Metal command buffer"};
      auto* renderer = model.GetRenderer<Renderer>();
      auto projection = model.Projection();
      renderer->SetMvpMatrix(&projection);
      renderer->SetRenderViewport({0, 0, static_cast<double>(width), static_cast<double>(height), 0, 1});
      renderer->StartFrame(command, pass);
      renderer->DrawModel();
      [command commit];
      [command waitUntilCompleted];
      if (command.status == MTLCommandBufferStatusError) {
        return {PlaybackErrorCode::BackendFailed, command.error.localizedDescription.UTF8String};
      }
      try {
        // The current macOS importer flips top-left Metal frames; compensate at publication.
        mailbox_->Publish({target_, Mailbox::Origin::BottomLeft, Mailbox::Alpha::Premultiplied});
      } catch (const std::runtime_error& error) {
        return {PlaybackErrorCode::BackendFailed, error.what()};
      }
      return {};
    }
  }

private:
  NativeModel& model_;
  std::shared_ptr<Mailbox> mailbox_;
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLTexture> target_;
  NSArray<id<MTLTexture>>* textures_;
};

} // namespace

std::unique_ptr<PlatformSurface> CreatePlatformSurface(const AssetData& asset, NativeModel& model) {
  return std::make_unique<MetalSurface>(asset, model);
}

} // namespace huxerui::live2d::detail
