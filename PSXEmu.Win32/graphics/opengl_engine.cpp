/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#include "graphics/opengl_engine.h"

#include "shaders/glsl_filters.h"
#include "tools/letterbox.h"

#include <algorithm>
#include <cmath>

namespace psxemu {

    namespace {

        // The same big triangle D3D12GraphicsEngine draws, with uv (0,0) at the top left of the
        // picture, stretched over u_rect - the picture's place in the target, in pixels counted
        // from the top left, fractions and all. D3D takes a fractional viewport; GL's is whole
        // pixels, and rounding it moved every column of the picture by a fraction of a pixel.
        // So the viewport stays the whole target and the triangle is placed here instead.
        //
        // Into a texture it is drawn upside down (u_flip_y = -1), so that texture row 0 - GL's
        // bottom - holds uv.y = 0 and the next pass reads it the same way round as the frame
        // uploaded top line first.
        const char kVertexShader[] = R"GLSL(#version 330 core
uniform float u_flip_y;
uniform vec4 u_rect;     // x, y, width, height in the target, top-down
uniform vec2 u_target;   // the target's size
out vec2 v_uv;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = uv;
    vec2 ndc = (u_rect.xy + uv * u_rect.zw) / u_target * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -u_flip_y * ndc.y, 0.0, 1.0);
}
)GLSL";

        // The texture units a draw's input sits on, one per sampler - see kGlslFilterHeader.
        const char* const kSamplerUniforms[5] = { "u_point", "u_linear", "u_point_clamp",
                                                  "u_linear_clamp", "u_original" };
        const int kOriginalUnit = 4;

    }   // namespace

    OpenGLGraphicsEngine::~OpenGLGraphicsEngine() {
        Shutdown();
    }

    bool OpenGLGraphicsEngine::CreateContext() {
        dc_ = GetDC(window_);
        if (dc_ == nullptr)
            return false;

        // A window takes one pixel format for its life. If an earlier OpenGL engine on this surface
        // already set one (a switch away and back), it is ours and is used as it is.
        if (GetPixelFormat(dc_) == 0) {
            PIXELFORMATDESCRIPTOR format = {};
            format.nSize = sizeof(format);
            format.nVersion = 1;
            format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            format.iPixelType = PFD_TYPE_RGBA;
            format.cColorBits = 32;
            format.cAlphaBits = 8;
            format.iLayerType = PFD_MAIN_PLANE;
            const int chosen = ChoosePixelFormat(dc_, &format);
            if (chosen == 0 || !SetPixelFormat(dc_, chosen, &format))
                return false;
        }

        // A plain context first, only to ask the driver for wglCreateContextAttribsARB; then the
        // 3.3 core context the engine actually uses.
        HGLRC legacy = wglCreateContext(dc_);
        if (legacy == nullptr || !wglMakeCurrent(dc_, legacy)) {
            if (legacy != nullptr)
                wglDeleteContext(legacy);
            return false;
        }
        gl_.LoadCreateContext();
        if (gl_.CreateContextAttribs != nullptr) {
            const int attributes[] = { kWglContextMajorVersion, 3,
                                       kWglContextMinorVersion, 3,
                                       kWglContextProfileMask,  kWglContextCoreProfileBit,
                                       kWglContextFlags,        kWglContextForwardCompatibleBit,
                                       0 };
            context_ = gl_.CreateContextAttribs(dc_, nullptr, attributes);
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (context_ == nullptr || !wglMakeCurrent(dc_, context_))
            return false;
        return gl_.Load();
    }

    bool OpenGLGraphicsEngine::Initialize(HWND window, int width, int height) {
        window_ = window;
        width_ = width > 0 ? width : 640;
        height_ = height > 0 ? height : 480;
        if (!CreateContext()) {
            Shutdown();
            return false;
        }

        // Core profile draws nothing without a vertex array bound, even one with no attributes.
        gl_.GenVertexArrays(1, &vertex_array_);
        gl_.BindVertexArray(vertex_array_);

        // D3D12GraphicsEngine's s0-s3. Its s0 and s1 wrap (CD3DX12_STATIC_SAMPLER_DESC's
        // default), which a filter reading past the edge of the picture sees.
        gl_.GenSamplers(4, samplers_);
        const GLint filters[4] = { GL_NEAREST, GL_LINEAR, GL_NEAREST, GL_LINEAR };
        const GLint wraps[4] = { GL_REPEAT, GL_REPEAT, static_cast<GLint>(kGlClampToEdge),
                                 static_cast<GLint>(kGlClampToEdge) };
        for (int i = 0; i < 4; ++i) {
            gl_.SamplerParameteri(samplers_[i], GL_TEXTURE_MIN_FILTER, filters[i]);
            gl_.SamplerParameteri(samplers_[i], GL_TEXTURE_MAG_FILTER, filters[i]);
            gl_.SamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_S, wraps[i]);
            gl_.SamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_T, wraps[i]);
            gl_.BindSampler(static_cast<GLuint>(i), samplers_[i]);
        }
        gl_.BindSampler(kOriginalUnit, samplers_[2]);

        if (!Compile(kGlslDefault, &default_) || !Compile(kGlslBlit, &blit_)) {
            Shutdown();
            return false;
        }
        current_ = &default_;
        SetVsync(vsync_);
        // The surface is the UI thread's window, hidden while a Direct3D engine draws. Posted
        // rather than sent: this thread must never wait on that one (Docs/Threading-Plan.md).
        ShowWindowAsync(window_, SW_SHOWNA);
        return true;
    }

    void OpenGLGraphicsEngine::Shutdown() {
        if (context_ != nullptr && wglMakeCurrent(dc_, context_)) {
            ReleaseChainTargets();
            for (auto& shader : shaders_)
                DeleteProgram(&shader.second);
            DeleteProgram(&default_);
            DeleteProgram(&blit_);
            if (frame_texture_ != 0)
                glDeleteTextures(1, &frame_texture_);
            if (samplers_[0] != 0)
                gl_.DeleteSamplers(4, samplers_);
            if (vertex_array_ != 0)
                gl_.DeleteVertexArrays(1, &vertex_array_);
        }
        shaders_.clear();
        chains_.clear();
        current_ = nullptr;
        active_chain_ = nullptr;
        frame_texture_ = 0;
        frame_width_ = frame_height_ = 0;
        vertex_array_ = 0;
        for (GLuint& sampler : samplers_)
            sampler = 0;

        if (context_ != nullptr) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context_);
            context_ = nullptr;
        }
        if (dc_ != nullptr) {
            ReleaseDC(window_, dc_);
            dc_ = nullptr;
        }
        // Out of the way of whichever Direct3D engine draws next.
        if (window_ != nullptr)
            ShowWindowAsync(window_, SW_HIDE);
    }

    // -------------------------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------------------------

    bool OpenGLGraphicsEngine::Compile(const char* fragment_body, Program* program) {
        auto compile = [this](GLenum type, const char* const* sources, GLsizei count) -> GLuint {
            const GLuint shader = gl_.CreateShader(type);
            gl_.ShaderSource(shader, count, sources, nullptr);
            gl_.CompileShader(shader);
            GLint ok = 0;
            gl_.GetShaderiv(shader, kGlCompileStatus, &ok);
            if (!ok) {
                char log[2048] = {};
                gl_.GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                OutputDebugStringA("PSXEmu OpenGL: a shader did not compile:\n");
                OutputDebugStringA(log);
                gl_.DeleteShader(shader);
                return 0;
            }
            return shader;
        };

        const char* vertex_sources[1] = { kVertexShader };
        const char* fragment_sources[2] = { kGlslFilterHeader, fragment_body };
        const GLuint vertex = compile(kGlVertexShader, vertex_sources, 1);
        const GLuint fragment = compile(kGlFragmentShader, fragment_sources, 2);
        if (vertex == 0 || fragment == 0) {
            if (vertex != 0)
                gl_.DeleteShader(vertex);
            if (fragment != 0)
                gl_.DeleteShader(fragment);
            return false;
        }

        const GLuint id = gl_.CreateProgram();
        gl_.AttachShader(id, vertex);
        gl_.AttachShader(id, fragment);
        gl_.LinkProgram(id);
        gl_.DeleteShader(vertex);
        gl_.DeleteShader(fragment);
        GLint linked = 0;
        gl_.GetProgramiv(id, kGlLinkStatus, &linked);
        if (!linked) {
            char log[2048] = {};
            gl_.GetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
            OutputDebugStringA("PSXEmu OpenGL: a shader did not link:\n");
            OutputDebugStringA(log);
            gl_.DeleteProgram(id);
            return false;
        }

        program->id = id;
        program->params = gl_.GetUniformLocation(id, "u_params");
        program->flip_y = gl_.GetUniformLocation(id, "u_flip_y");
        program->rect = gl_.GetUniformLocation(id, "u_rect");
        program->target = gl_.GetUniformLocation(id, "u_target");
        program->frag_y = gl_.GetUniformLocation(id, "u_frag_y");
        // The units never change, so the sampler uniforms are set once here. One a shader does
        // not use is optimised out, and setting location -1 does nothing.
        gl_.UseProgram(id);
        for (int unit = 0; unit < 5; ++unit)
            gl_.Uniform1i(gl_.GetUniformLocation(id, kSamplerUniforms[unit]), unit);
        gl_.UseProgram(0);
        return true;
    }

    void OpenGLGraphicsEngine::DeleteProgram(Program* program) {
        if (program->id != 0)
            gl_.DeleteProgram(program->id);
        *program = Program();
    }

    bool OpenGLGraphicsEngine::LoadPixelShaderFromString(const std::string& name,
                                                         const char* source) {
        if (context_ == nullptr || name.empty() || source == nullptr)
            return false;
        Program program;
        if (!Compile(source, &program))
            return false;
        auto existing = shaders_.find(name);
        if (existing != shaders_.end()) {
            if (current_ == &existing->second)
                current_ = &default_;
            DeleteProgram(&existing->second);
        }
        shaders_[name] = program;
        return true;
    }

    bool OpenGLGraphicsEngine::LoadShaderChain(const std::string& name,
                                               const std::vector<ShaderPass>& passes) {
        if (name.empty() || passes.empty())
            return false;
        Chain chain;
        for (size_t i = 0; i < passes.size(); ++i) {
            // Only the last pass may draw straight into the window, as in D3D12.
            if (passes[i].scale < 0 || (passes[i].scale == 0 && i + 1 != passes.size()))
                return false;
            if (shaders_.find(passes[i].shader) == shaders_.end())
                return false;
            chain.passes.push_back(passes[i].shader);
            chain.scales.push_back(passes[i].scale);
        }
        if (active_chain_ == &chains_[name])
            active_chain_ = nullptr;
        ReleaseChainTargets();
        chains_[name] = chain;
        return true;
    }

    void OpenGLGraphicsEngine::SetPixelShader(const std::string& name) {
        active_chain_ = nullptr;
        current_ = &default_;
        if (name.empty())
            return;
        const auto chain = chains_.find(name);
        if (chain != chains_.end()) {
            active_chain_ = &chain->second;
            return;
        }
        const auto shader = shaders_.find(name);
        if (shader != shaders_.end())
            current_ = &shader->second;
    }

    void OpenGLGraphicsEngine::SetVsync(bool enabled) {
        vsync_ = enabled;
        if (context_ != nullptr && gl_.SwapInterval != nullptr)
            gl_.SwapInterval(enabled ? 1 : 0);
    }

    // -------------------------------------------------------------------------------------------
    // Frames
    // -------------------------------------------------------------------------------------------

    bool OpenGLGraphicsEngine::EnsureFrameTexture(int width, int height) {
        if (frame_texture_ != 0 && width == frame_width_ && height == frame_height_)
            return true;
        if (frame_texture_ == 0)
            glGenTextures(1, &frame_texture_);
        glBindTexture(GL_TEXTURE_2D, frame_texture_);
        glTexParameteri(GL_TEXTURE_2D, kGlTextureMaxLevel, 0);
        // BGRA bytes, which is how Gpu::ResolveFramebuffer packs a pixel - no swizzle anywhere.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, kGlBgra, GL_UNSIGNED_BYTE,
                     nullptr);
        frame_width_ = width;
        frame_height_ = height;
        return true;
    }

    void OpenGLGraphicsEngine::ReleaseChainTargets() {
        if (context_ != nullptr) {
            if (!chain_framebuffers_.empty())
                gl_.DeleteFramebuffers(static_cast<GLsizei>(chain_framebuffers_.size()),
                                       chain_framebuffers_.data());
            if (!chain_textures_.empty())
                glDeleteTextures(static_cast<GLsizei>(chain_textures_.size()),
                                 chain_textures_.data());
        }
        chain_framebuffers_.clear();
        chain_textures_.clear();
        chain_widths_.clear();
        chain_heights_.clear();
        chain_targets_for_ = nullptr;
    }

    // One texture per pass that renders at a multiple of the frame, remade when the chain or the
    // frame's size changes - the PSX changes resolution mid-boot.
    bool OpenGLGraphicsEngine::EnsureChainTargets(const Chain& chain, int width, int height) {
        if (chain_targets_for_ == &chain && chain_source_width_ == width &&
            chain_source_height_ == height)
            return true;
        ReleaseChainTargets();
        // A pass's scale is a multiple of the emulator's frame, not of the pass before it
        // (ShaderPass), so Super-xBR's three passes are all twice the frame.
        for (const int scale : chain.scales) {
            if (scale == 0)
                break;
            const int out_width = width * scale;
            const int out_height = height * scale;
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, kGlTextureMaxLevel, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, out_width, out_height, 0, kGlBgra,
                         GL_UNSIGNED_BYTE, nullptr);
            GLuint framebuffer = 0;
            gl_.GenFramebuffers(1, &framebuffer);
            gl_.BindFramebuffer(kGlFramebuffer, framebuffer);
            gl_.FramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, texture, 0);
            const bool complete = gl_.CheckFramebufferStatus(kGlFramebuffer) == kGlFramebufferComplete;
            gl_.BindFramebuffer(kGlFramebuffer, 0);
            chain_textures_.push_back(texture);
            chain_framebuffers_.push_back(framebuffer);
            chain_widths_.push_back(out_width);
            chain_heights_.push_back(out_height);
            if (!complete) {
                ReleaseChainTargets();
                return false;
            }
        }
        chain_targets_for_ = &chain;
        chain_source_width_ = width;
        chain_source_height_ = height;
        return true;
    }

    void OpenGLGraphicsEngine::Draw(const Program& program, GLuint input, int target,
                                    float out_width, float out_height, float in_width,
                                    float in_height) {
        const bool to_window = target == 0;
        gl_.BindFramebuffer(kGlFramebuffer, to_window ? 0 : chain_framebuffers_[target - 1]);
        const float target_w = static_cast<float>(to_window ? width_ : chain_widths_[target - 1]);
        const float target_h = static_cast<float>(to_window ? height_ : chain_heights_[target - 1]);
        glViewport(0, 0, static_cast<GLsizei>(target_w), static_cast<GLsizei>(target_h));
        if (to_window) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor_x_, scissor_y_, scissor_width_, scissor_height_);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        gl_.UseProgram(program.id);
        for (int unit = 0; unit < 4; ++unit) {
            gl_.ActiveTexture(kGlTexture0 + unit);
            glBindTexture(GL_TEXTURE_2D, input);
        }
        gl_.ActiveTexture(kGlTexture0 + kOriginalUnit);
        glBindTexture(GL_TEXTURE_2D, frame_texture_);

        gl_.Uniform4f(program.params, out_width, out_height, in_width, in_height);
        gl_.Uniform1f(program.flip_y, to_window ? 1.0f : -1.0f);
        gl_.Uniform2f(program.target, target_w, target_h);
        if (to_window)
            gl_.Uniform4f(program.rect, rect_x_, rect_y_, rect_width_, rect_height_);
        else
            gl_.Uniform4f(program.rect, 0.0f, 0.0f, target_w, target_h);
        // SV_Position counts down from the top of the target; gl_FragCoord up from the bottom.
        // Into a texture the picture is already stored upside down, so the two agree there.
        if (to_window)
            gl_.Uniform2f(program.frag_y, static_cast<float>(height_), -1.0f);
        else
            gl_.Uniform2f(program.frag_y, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void OpenGLGraphicsEngine::BeginFrame() {
        if (context_ == nullptr)
            return;
        gl_.BindFramebuffer(kGlFramebuffer, 0);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width_, height_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void OpenGLGraphicsEngine::RenderFramebuffer(const void* data, int width, int height) {
        if (context_ == nullptr || data == nullptr || width <= 0 || height <= 0)
            return;
        if (!EnsureFrameTexture(width, height))
            return;
        glBindTexture(GL_TEXTURE_2D, frame_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, kGlBgra, GL_UNSIGNED_BYTE, data);

        // A fixed 4:3, as both Direct3D engines draw it (bug 47), placed exactly where D3D12 puts
        // it. Its viewport is the letterbox, fractions and all, so it draws the pixels whose
        // centres fall inside; its scissor is the letterbox cut down to whole pixels. The pixels
        // both allow are this scissor, and the vertex shader puts the picture over the fractional
        // rectangle, so each column samples the texel it would there.
        const LetterboxRect rect = ComputeLetterboxRect(width_, height_, 4.0f / 3.0f);
        rect_x_ = rect.x;
        rect_y_ = rect.y;
        rect_width_ = rect.width;
        rect_height_ = rect.height;
        const int left = (std::max)(static_cast<int>(std::ceil(rect.x - 0.5f)),
                                  static_cast<int>(rect.x));
        const int right = (std::min)(static_cast<int>(std::ceil(rect.x + rect.width - 0.5f)),
                                   static_cast<int>(rect.x + rect.width));
        const int top = (std::max)(static_cast<int>(std::ceil(rect.y - 0.5f)),
                                 static_cast<int>(rect.y));
        const int bottom = (std::min)(static_cast<int>(std::ceil(rect.y + rect.height - 0.5f)),
                                    static_cast<int>(rect.y + rect.height));
        scissor_x_ = left;
        scissor_width_ = (std::max)(right - left, 0);
        scissor_height_ = (std::max)(bottom - top, 0);
        scissor_y_ = height_ - bottom;

        const float frame_w = static_cast<float>(width);
        const float frame_h = static_cast<float>(height);

        // A chain draws every pass itself, the last into the letterbox. If its targets cannot be
        // made, the frame is drawn plain instead, as D3D12 does.
        if (active_chain_ != nullptr && EnsureChainTargets(*active_chain_, width, height)) {
            GLuint input = frame_texture_;
            float in_w = frame_w;
            float in_h = frame_h;
            const size_t passes = active_chain_->passes.size();
            for (size_t i = 0; i < passes; ++i) {
                const Program& program = shaders_[active_chain_->passes[i]];
                if (active_chain_->scales[i] == 0) {
                    Draw(program, input, 0, rect.width, rect.height, in_w, in_h);
                    return;
                }
                const int target = static_cast<int>(i) + 1;
                const float out_w = static_cast<float>(chain_widths_[i]);
                const float out_h = static_cast<float>(chain_heights_[i]);
                Draw(program, input, target, out_w, out_h, in_w, in_h);
                input = chain_textures_[i];
                in_w = out_w;
                in_h = out_h;
            }
            // The last pass rendered at a multiple of the frame: stretch it onto the window.
            Draw(blit_, input, 0, rect.width, rect.height, in_w, in_h);
            return;
        }

        // outW/outH are the letterbox's size, not the window's - Sharp Bilinear works out its
        // texel scale from them.
        Draw(current_ != nullptr ? *current_ : default_, frame_texture_, 0, rect.width,
             rect.height, frame_w, frame_h);
    }

    void OpenGLGraphicsEngine::EndFrame() {
        if (context_ == nullptr)
            return;
        SwapBuffers(dc_);
    }

    void OpenGLGraphicsEngine::Resize(int width, int height) {
        if (width <= 0 || height <= 0)
            return;
        // The default framebuffer follows the window by itself; only the viewport needs to know.
        width_ = width;
        height_ = height;
    }

}   // namespace psxemu
