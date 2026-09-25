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
#include "graphics/vk_functions.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace psxemu {

    /*
      The fourth presenter: Vulkan 1.0, loaded from the driver's vulkan-1.dll at run time.

      Laid out like D3D12GraphicsEngine and OpenGLGraphicsEngine so all three can be held up
      against each other: the frame is copied into a texture and drawn into the 4:3 letterbox -
      with a fractional viewport and D3D12's scissor, which Vulkan, unlike OpenGL, can express
      directly - every filter is a fragment shader, and a chain (Super-xBR) renders each pass into
      a texture at a multiple of the frame before a linear blit puts the last one on screen.

      Its shaders are the GLSL the OpenGL engine uses, compiled to SPIR-V ahead of time
      (shaders/spirv_filters.h), because Vulkan takes nothing else. So LoadCustomPixelShader takes
      SPIR-V, and LoadPixelShaderFromString, which would need a compiler here, says no. The five
      samplers are D3D12's four on bindings 0-3, all over the same input, and the untouched frame
      on binding 4.

      One frame in flight: each frame waits for the last to finish on the GPU before it reuses the
      command buffer and the upload buffer. A presenter's GPU work is a copy and a few full-screen
      triangles, and with vsync the wait is for the display anyway.

      It draws into a child window of its own (App::CreateRenderSurfaces says why), shown when it
      starts and hidden when it stops. Everything runs on the video thread.
    */
    class VulkanGraphicsEngine : public IGraphicsEngine {
     public:
        VulkanGraphicsEngine() = default;
        ~VulkanGraphicsEngine() override;

        bool Initialize(HWND window, int width, int height) override;
        void Shutdown() override;

        void BeginFrame() override {}
        void RenderFramebuffer(const void* data, int width, int height) override;
        void EndFrame() override;
        void Resize(int width, int height) override;

        void SetVsync(bool enabled) override;
        void SetPixelShader(const std::string& name) override;
        // `bytecode` is SPIR-V (spirv_filters.h).
        bool LoadCustomPixelShader(const std::string& name, const uint8_t* bytecode,
                                   size_t size) override;
        // GLSL would need a compiler at run time; the shaders arrive compiled instead.
        bool LoadPixelShaderFromString(const std::string&, const char*) override { return false; }
        bool LoadShaderChain(const std::string& name, const std::vector<ShaderPass>& passes) override;

     private:
        struct Shader {
            VkShaderModule module = nullptr;
            VkPipeline pipeline = nullptr;
        };

        struct Chain {
            std::vector<std::string> passes;
            std::vector<int> scales;
        };

        // An image the engine draws into or reads: a chain pass's target, or the frame itself.
        struct Image {
            VkImage image = nullptr;
            VkDeviceMemory memory = nullptr;
            VkImageView view = nullptr;
            VkFramebuffer framebuffer = nullptr;   // chain targets only
            VkDescriptorSet reads = nullptr;       // this image as a draw's input
            int width = 0;
            int height = 0;
        };

        bool CreateDevice();
        bool CreateRenderPasses();
        bool CreatePipelineObjects();
        bool CreateSwapchain();
        void DestroySwapchain();
        bool CreateShader(const uint32_t* code, size_t bytes, Shader* shader);
        void DestroyShader(Shader* shader);
        bool CreateImage(int width, int height, VkFormat format, VkFlags usage, Image* image);
        void DestroyImage(Image* image);
        bool EnsureFrameTexture(int width, int height);
        void ReleaseFrameTexture();
        bool EnsureChainTargets(const Chain& chain, int width, int height);
        void ReleaseChainTargets();
        VkDescriptorSet AllocateReads(VkDescriptorPool pool, VkImageView input);
        int FindMemory(uint32_t type_bits, VkFlags properties) const;
        void Draw(VkCommandBuffer commands, const Shader& shader, VkDescriptorSet reads,
                  float out_width, float out_height, float in_width, float in_height);

        HWND window_ = nullptr;
        int width_ = 0;
        int height_ = 0;
        bool vsync_ = true;
        VkFunctions vk_;

        VkInstance instance_ = nullptr;
        VkSurfaceKHR surface_ = nullptr;
        VkPhysicalDevice gpu_ = nullptr;
        VkPhysicalDeviceMemoryProperties memory_ = {};
        uint32_t queue_family_ = 0;
        VkDevice device_ = nullptr;
        VkQueue queue_ = nullptr;

        VkFormat format_ = VK_FORMAT_UNDEFINED;       // the swap chain's, and the chain targets'
        VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkSwapchainKHR swapchain_ = nullptr;
        VkExtent2D extent_ = {};
        std::vector<VkImage> swap_images_;
        std::vector<VkImageView> swap_views_;
        std::vector<VkFramebuffer> swap_framebuffers_;
        std::vector<VkSemaphore> rendered_;           // one per swap-chain image
        bool swapchain_dirty_ = false;

        VkRenderPass window_pass_ = nullptr;          // clears, ends ready to present
        VkRenderPass target_pass_ = nullptr;          // a chain pass, ends ready to sample
        VkSampler samplers_[4] = {};                  // point wrap, linear wrap, point/linear clamp
        VkDescriptorSetLayout set_layout_ = nullptr;
        VkPipelineLayout pipeline_layout_ = nullptr;
        VkShaderModule vertex_ = nullptr;
        Shader default_;
        Shader blit_;
        std::unordered_map<std::string, Shader> shaders_;
        std::unordered_map<std::string, Chain> chains_;
        const Shader* current_ = nullptr;
        const Chain* active_chain_ = nullptr;

        VkCommandPool command_pool_ = nullptr;
        VkCommandBuffer commands_ = nullptr;
        VkFence frame_done_ = nullptr;
        VkSemaphore acquired_ = nullptr;
        uint32_t pending_image_ = 0;
        bool pending_present_ = false;

        // The frame, and the buffer it is copied up through.
        Image frame_;
        VkImageLayout frame_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        VkBuffer staging_ = nullptr;
        VkDeviceMemory staging_memory_ = nullptr;
        void* staging_mapped_ = nullptr;
        VkDescriptorPool frame_pool_ = nullptr;

        // The active chain's targets, for one frame size.
        std::vector<Image> targets_;
        VkDescriptorPool chain_pool_ = nullptr;
        const Chain* targets_for_ = nullptr;
        int targets_width_ = 0;
        int targets_height_ = 0;
    };

}   // namespace psxemu
