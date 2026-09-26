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
#pragma once

#include "graphics/igraphicsengine.h"
#include "graphics/gl_functions.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace psxemu {

    /*
      The third presenter: OpenGL 3.3 core, on the window through WGL.

      It does what D3D12GraphicsEngine does, arranged the same way, so the two can be held up
      against each other pixel for pixel: the frame uploaded into a texture and drawn into a 4:3
      letterbox, every filter a fragment shader, and a filter chain (Super-xBR) rendering each
      pass into a texture at a multiple of the frame before a linear blit puts the last on screen.

      Filters are GLSL here - shaders/glsl_filters.h holds a port of every HLSL one - so
      LoadPixelShaderFromString takes GLSL, and LoadCustomPixelShader, which is HLSL bytecode,
      says no. The ports keep the HLSL shaders' samplers: D3D12's four static samplers (point and
      linear, each wrapping or clamped) become four GL sampler objects on four texture units that
      all hold the same input, so a shader that sampled s0 still wraps at the edge and one that
      sampled s2 still clamps. The untouched frame, t1 in HLSL, is on a fifth unit.

      It draws into a child window of its own, not the main window (App::CreateGlSurface says
      why), which it shows when it starts and hides when it stops.

      Everything runs on the video thread, which owns the context from Initialize to Shutdown.
    */
    class OpenGLGraphicsEngine : public IGraphicsEngine {
     public:
        OpenGLGraphicsEngine() = default;
        ~OpenGLGraphicsEngine() override;

        bool Initialize(HWND window, int width, int height) override;
        void Shutdown() override;

        void BeginFrame() override;
        void RenderFramebuffer(const void* data, int width, int height) override;
        void EndFrame() override;
        void Resize(int width, int height) override;

        void SetVsync(bool enabled) override;
        void SetPixelShader(const std::string& name) override;
        // HLSL bytecode means nothing to OpenGL.
        bool LoadCustomPixelShader(const std::string&, const uint8_t*, size_t) override {
            return false;
        }
        // `source` is the body of a GLSL fragment shader; see kGlslFilterHeader for what it can
        // use.
        bool LoadPixelShaderFromString(const std::string& name, const char* source) override;
        bool LoadShaderChain(const std::string& name, const std::vector<ShaderPass>& passes) override;
        void SetOverlay(const OverlayDrawData* overlay) override { overlay_ = overlay; }

     private:
        // ---- the overlay (ui/overlay), drawn last in EndFrame ------------------------------
        bool CreateOverlayPipeline();
        void DrawOverlay();
        void ReleaseOverlay();
        const OverlayDrawData* overlay_ = nullptr;
        GLuint overlay_program_ = 0;
        GLuint overlay_vertex_array_ = 0;
        GLuint overlay_vertices_ = 0;
        GLuint overlay_indices_ = 0;
        GLuint overlay_atlas_ = 0;
        GLuint overlay_sampler_ = 0;
        uint64_t overlay_atlas_version_ = 0;
        std::vector<OverlayVertex> overlay_scratch_;

        struct Program {
            GLuint id = 0;
            GLint params = -1;   // u_params: outW, outH, inW, inH
            GLint flip_y = -1;   // u_flip_y: 1 drawing to the window, -1 into a texture
            GLint frag_y = -1;   // u_frag_y: turns gl_FragCoord.y into D3D's top-down y
            GLint rect = -1;     // u_rect: where the picture goes, in target pixels
            GLint target = -1;   // u_target: the target's size
        };

        struct Chain {
            std::vector<std::string> passes;
            std::vector<int> scales;
        };

        bool CreateContext();
        bool Compile(const char* fragment_body, Program* program);
        void DeleteProgram(Program* program);
        bool EnsureFrameTexture(int width, int height);
        bool EnsureChainTargets(const Chain& chain, int width, int height);
        void ReleaseChainTargets();
        // One full-screen draw with `program`, reading `input`: into the window's letterbox
        // (`target` 0) or into chain target `target` - 1.
        void Draw(const Program& program, GLuint input, int target, float out_width,
                  float out_height, float in_width, float in_height);

        HWND window_ = nullptr;
        HDC dc_ = nullptr;
        HGLRC context_ = nullptr;
        GlFunctions gl_;
        int width_ = 0;
        int height_ = 0;
        bool vsync_ = true;

        GLuint vertex_array_ = 0;
        GLuint samplers_[4] = {};   // point wrap, linear wrap, point clamp, linear clamp
        GLuint frame_texture_ = 0;
        int frame_width_ = 0;
        int frame_height_ = 0;

        Program default_;   // point sampling, no filter
        Program blit_;      // a chain's last texture onto the window, linear
        std::unordered_map<std::string, Program> shaders_;
        std::unordered_map<std::string, Chain> chains_;
        const Program* current_ = nullptr;
        const Chain* active_chain_ = nullptr;

        // The active chain's render targets, for one frame size.
        std::vector<GLuint> chain_textures_;
        std::vector<GLuint> chain_framebuffers_;
        std::vector<int> chain_widths_;
        std::vector<int> chain_heights_;
        const Chain* chain_targets_for_ = nullptr;
        int chain_source_width_ = 0;
        int chain_source_height_ = 0;

        // Where the picture goes this frame: the letterbox, as D3D12 is given it, and the whole
        // pixels its viewport and scissor together let it draw, in GL's bottom-up coordinates.
        float rect_x_ = 0.0f, rect_y_ = 0.0f, rect_width_ = 0.0f, rect_height_ = 0.0f;
        int scissor_x_ = 0, scissor_y_ = 0, scissor_width_ = 0, scissor_height_ = 0;
    };

}   // namespace psxemu
