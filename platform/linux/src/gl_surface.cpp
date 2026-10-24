#include "native/native_model.h"
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <stdexcept>

#include <gdk/gdk.h>
#include <huxerui/linux/external_texture.h>

namespace huxerui::live2d::detail {
namespace {

using Renderer = cubism::Rendering::CubismRenderer_OpenGLES2;
using Mailbox = huxerui::linux::GlTexture;

class Context {
public:
  Context() {
    auto* display = gdk_display_get_default();
    if (!display) throw std::runtime_error("No GDK display is available");
    GError* error = nullptr;
    context = gdk_display_create_gl_context(display, &error);
    if (context) {
      // HuxerUI GlTexture requires ES 3.1+ and GL_RGBA8 storage. The Linux
      // Cubism framework is patched to upload all mesh and quad data to VBOs,
      // since ES 3 forbids the client-side arrays used by its GLES2 renderer.
      gdk_gl_context_set_use_es(context, true);
      gdk_gl_context_set_required_version(context, 3, 1);
    }
    if (!context || !gdk_gl_context_realize(context, &error)) {
      const std::string message = error ? error->message : "Cannot create a GDK GL context";
      if (error) g_error_free(error);
      if (context) g_object_unref(context);
      throw std::runtime_error(message);
    }
  }
  ~Context() { g_object_unref(context); }
  GdkGLContext* context = nullptr;
};

class CurrentContext {
public:
  explicit CurrentContext(Context& context) : previous_(gdk_gl_context_get_current()) {
    if (previous_) g_object_ref(previous_);
    gdk_gl_context_make_current(context.context);
    if (gdk_gl_context_get_current() != context.context) throw std::runtime_error("Cannot make GDK GL context current");
    // GLEW 在 Wayland 等 GLX 缺席的会话返回 NO_GLX_DISPLAY，但它只是
    // 没法建 GLX 扩展表；Cubism 渲染器直接调用链接期解析的 GL 核心符号
    // （桌面 libGL 3.2+ 全部导出），不经过 GLEW 函数表——该错误可容忍。
    // 其余失败仍视为不可用。
    glewExperimental = GL_TRUE;
    const GLenum glew = glewInit();
    if (glew != GLEW_OK && glew != GLEW_ERROR_NO_GLX_DISPLAY) {
      throw std::runtime_error("Cannot initialize OpenGL functions");
    }
    while (glGetError() != GL_NO_ERROR) {}
  }
  ~CurrentContext() {
    if (previous_) { gdk_gl_context_make_current(previous_); g_object_unref(previous_); }
    else gdk_gl_context_clear_current();
  }
private:
  GdkGLContext* previous_;
};

std::shared_ptr<Context> AcquireContext() {
  static std::weak_ptr<Context> shared;
  auto context = shared.lock();
  if (!context) { context = std::make_shared<Context>(); shared = context; }
  return context;
}

class GlSurface final : public NativeSurface {
public:
  GlSurface(const AssetData& asset, NativeModel& model)
      : context_(AcquireContext()), model_(model), mailbox_(std::make_shared<Mailbox>(asset.info.canvas)) {
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
      glGenVertexArrays(1, &vertex_array_);
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
      glBindVertexArray(vertex_array_);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
      if (pixels != pixels_) {
        glBindTexture(GL_TEXTURE_2D, target_);
        ConfigureTexture();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
          return {PlaybackErrorCode::BackendFailed, "Incomplete OpenGL framebuffer"};
        }
        model.SetRenderTargetSize(width, height);
        pixels_ = pixels;
      }
      glViewport(0, 0, width, height);
      glDisable(GL_SCISSOR_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
      // 诊断：清掉历史错误，分段定位 0x502 的来源（我们的调用 vs DrawModel）。
      while (glGetError() != GL_NO_ERROR) {}
      auto projection = model.Projection();
      auto* renderer = model.GetRenderer<Renderer>();
      renderer->SetMvpMatrix(&projection);
      const GLenum preDraw = glGetError();
      renderer->DrawModel();
      const GLenum drawError = glGetError();
      if (preDraw != GL_NO_ERROR || drawError != GL_NO_ERROR) {
        return {PlaybackErrorCode::BackendFailed,
                "OpenGL rendering failed (pre=0x" +
                    [&] {
                      char buffer[16];
                      std::snprintf(buffer, sizeof(buffer), "%X", preDraw);
                      return std::string(buffer);
                    }() + ", draw=0x" +
                    [&] {
                      char buffer[16];
                      std::snprintf(buffer, sizeof(buffer), "%X", drawError);
                      return std::string(buffer);
                    }() + ")"};
      }
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
    glDeleteVertexArrays(1, &vertex_array_);
    if (context_.use_count() == 1) Renderer::StaticRelease();
  }
  std::shared_ptr<Context> context_;
  NativeModel& model_;
  std::shared_ptr<Mailbox> mailbox_;
  std::vector<GLuint> textures_;
  GLuint target_ = 0, framebuffer_ = 0;
  GLuint vertex_array_ = 0;
  Size pixels_{};
};

} // namespace


std::unique_ptr<NativeSurface> CreateNativeSurface(const AssetData& asset, NativeModel& model) {
  return std::make_unique<GlSurface>(asset, model);
}

} // namespace huxerui::live2d::detail
