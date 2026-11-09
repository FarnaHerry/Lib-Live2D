#include "native/native_model.h"
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <stdexcept>

#include <EGL/egl.h>
#include <huxerui/android/external_texture.h>

namespace huxerui::live2d::detail {
namespace {

using Renderer = cubism::Rendering::CubismRenderer_OpenGLES2;
using Mailbox = huxerui::android::GlTexture;

struct WorkerRuntimeLease {
  std::shared_ptr<void> runtime;
  ~WorkerRuntimeLease() {
    // Serialize the last worker release with acquisition of the next Cubism runtime.
    std::lock_guard lock(CubismMutex());
    runtime.reset();
  }
};

class Context {
public:
  Context() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
      throw std::runtime_error("Cannot initialize EGL");
    }
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) {
      throw std::runtime_error("No compatible EGL configuration");
    }
    const EGLint size[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, size);
    const EGLint version[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT) {
      if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
      if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
      throw std::runtime_error("Cannot create the Cubism EGL context");
    }
  }
  ~Context();
  WorkerSequence preparation;
  bool prepared = false;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
};

class CurrentContext {
public:
  explicit CurrentContext(Context& context) : producer_(context.display), display_(eglGetCurrentDisplay()),
      context_(eglGetCurrentContext()), draw_(eglGetCurrentSurface(EGL_DRAW)), read_(eglGetCurrentSurface(EGL_READ)) {
    if (!eglMakeCurrent(context.display, context.surface, context.surface, context.context)) {
      throw std::runtime_error("Cannot make the Cubism EGL context current");
    }
  }
  ~CurrentContext() {
    if (display_ != EGL_NO_DISPLAY) eglMakeCurrent(display_, draw_, read_, context_);
    else eglMakeCurrent(producer_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
private:
  EGLDisplay producer_, display_;
  EGLContext context_;
  EGLSurface draw_, read_;
};

Context::~Context() {
  try {
    CurrentContext current(*this);
    Renderer::StaticRelease();
  } catch (...) {}
  eglDestroyContext(display, context);
  eglDestroySurface(display, surface);
}

std::shared_ptr<Context> AcquireContext(const std::shared_ptr<void>& runtime) {
  auto& shared = CubismGraphicsContext(runtime);
  if (!shared) shared = std::make_shared<Context>();
  return std::static_pointer_cast<Context>(shared);
}

class GlSurface final : public PlatformSurface {
public:
  GlSurface(const AssetData& asset, NativeModel& model)
      : context_(AcquireContext(asset.runtime)), model_(model), mailbox_(std::make_shared<Mailbox>(asset.info.canvas)) {
    CurrentContext current(*context_);
    try {
      model_.CreateRenderer(1, 1);
      auto* renderer = model_.GetRenderer<Renderer>();
      renderer->IsPremultipliedAlpha(false);
      textures_.resize(asset.manifest.textures.size());
      glGenTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
      for (std::size_t index = 0; index < textures_.size(); ++index) {
        const auto& decoded = asset.images.at(index);
        glBindTexture(GL_TEXTURE_2D, textures_[index]);
        ConfigureTexture();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, decoded.width, decoded.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, decoded.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        renderer->BindTexture(static_cast<int>(index), textures_[index]);
      }
      glGenTextures(1, &target_);
      glGenFramebuffers(1, &framebuffer_);
      if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot upload model textures to OpenGL");
    } catch (...) { Release(); throw; }
  }

  ~GlSurface() override {
    try { CurrentContext current(*context_); Release(); } catch (...) {}
    mailbox_->Finish();
  }

  std::shared_ptr<ExternalTexture> Texture() const override { return mailbox_; }

  PlaybackError Draw(NativeModel& model, Size pixels) override {
    try {
      CurrentContext current(*context_);
      const int width = static_cast<int>(pixels.width), height = static_cast<int>(pixels.height);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
      // Orphan storage retained by the previous EGLImage before publishing another frame.
      glBindTexture(GL_TEXTURE_2D, target_);
      ConfigureTexture();
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_, 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return {PlaybackErrorCode::BackendFailed, "Incomplete OpenGL framebuffer"};
      }
      model.SetRenderTargetSize(width, height);
      glViewport(0, 0, width, height);
      glDisable(GL_SCISSOR_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
      auto projection = model.Projection();
      auto* renderer = model.GetRenderer<Renderer>();
      renderer->SetMvpMatrix(&projection);
      renderer->DrawModel();
      if (glGetError() != GL_NO_ERROR) return {PlaybackErrorCode::BackendFailed, "OpenGL rendering failed"};
      mailbox_->PublishCurrent({.texture_name = target_, .pixel_width = width, .pixel_height = height,
                                .origin = Mailbox::Origin::BottomLeft, .alpha = Mailbox::Alpha::Premultiplied});
      return {};
    } catch (const std::runtime_error& error) {
      return {PlaybackErrorCode::BackendFailed, error.what()};
    }
  }

private:
  static void ConfigureTexture() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  void Release() {
    model_.DeleteRenderer();
    glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
    glDeleteTextures(1, &target_);
    glDeleteFramebuffers(1, &framebuffer_);
  }
  std::shared_ptr<Context> context_;
  NativeModel& model_;
  std::shared_ptr<Mailbox> mailbox_;
  std::vector<GLuint> textures_;
  GLuint target_ = 0, framebuffer_ = 0;
};

} // namespace

Task<void> PrepareGlContextAsync(std::shared_ptr<void> runtime) {
  auto context = AcquireContext(runtime);
  auto lease = std::make_shared<WorkerRuntimeLease>();
  lease->runtime = runtime;
  co_await context->preparation.Run([lease](std::stop_token stop) {
    auto* context = static_cast<Context*>(CubismGraphicsContext(lease->runtime).get());
    if (stop.stop_requested() || context->prepared) return;
    // The context is used by renderers only after this preparation task completes.
    CurrentContext current(*context);
    cubism::Rendering::CubismShader_OpenGLES2::GetInstance();
    glFinish();
    if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot prepare Cubism OpenGL shaders");
    context->prepared = true;
  });
}

std::unique_ptr<PlatformSurface> CreatePlatformSurface(const AssetData& asset, NativeModel& model) {
  return std::make_unique<GlSurface>(asset, model);
}

} // namespace huxerui::live2d::detail
