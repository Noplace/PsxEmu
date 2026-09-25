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
#include "graphics/vulkan_engine.h"

#include "shaders/spirv_filters.h"
#include "tools/letterbox.h"

#include <cstring>

namespace psxemu {

    namespace {

        constexpr uint64_t kForever = ~0ull;
        constexpr uint32_t kPushBytes = 4 * sizeof(float);   // u_params: outW, outH, inW, inH
        constexpr uint32_t kBindings = 5;                    // four samplers over the input, and t1

        // kSpirvShaders' first three: the vertex shader, the pass-through and the chain's blit.
        const SpirvShader& kVertexSpirv = kSpirvShaders[0];
        const SpirvShader& kDefaultSpirv = kSpirvShaders[1];
        const SpirvShader& kBlitSpirv = kSpirvShaders[2];

        VkImageSubresourceRange ColorRange() {
            VkImageSubresourceRange range = {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            return range;
        }

        bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
            for (const VkExtensionProperties& extension : list) {
                if (strcmp(extension.extensionName, name) == 0)
                    return true;
            }
            return false;
        }

    }   // namespace

    VulkanGraphicsEngine::~VulkanGraphicsEngine() {
        Shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    // Bringing it up
    // ---------------------------------------------------------------------------------------------

    bool VulkanGraphicsEngine::Initialize(HWND window, int width, int height) {
        window_ = window;
        width_ = width > 0 ? width : 640;
        height_ = height > 0 ? height : 480;
        if (!vk_.LoadLibraryAndGlobals() || !CreateDevice() || !CreateRenderPasses() ||
            !CreatePipelineObjects() || !CreateSwapchain()) {
            Shutdown();
            return false;
        }
        current_ = &default_;
        // The surface is the UI thread's window, hidden while another engine draws. Posted rather
        // than sent: this thread must never wait on that one (Docs/Threading-Plan.md).
        ShowWindowAsync(window_, SW_SHOWNA);
        return true;
    }

    bool VulkanGraphicsEngine::CreateDevice() {
        VkApplicationInfo application = {};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "PSXEmu";
        application.pEngineName = "PSXEmu";
        application.apiVersion = kVkApiVersion10;
        const char* instance_extensions[] = { kVkSurfaceExtension, kVkWin32SurfaceExtension };
        VkInstanceCreateInfo instance_info = {};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = 2;
        instance_info.ppEnabledExtensionNames = instance_extensions;
        if (vk_.CreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS ||
            !vk_.LoadInstance(instance_))
            return false;

        VkWin32SurfaceCreateInfoKHR surface_info = {};
        surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_info.hinstance = GetModuleHandleW(nullptr);
        surface_info.hwnd = window_;
        if (vk_.CreateWin32SurfaceKHR(instance_, &surface_info, nullptr, &surface_) != VK_SUCCESS)
            return false;

        // The GPU: one with a queue that both draws and presents to this window, and the
        // swap-chain extension - a discrete one over an integrated one if there are both.
        uint32_t count = 0;
        vk_.EnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> gpus(count);
        if (count == 0 || vk_.EnumeratePhysicalDevices(instance_, &count, gpus.data()) != VK_SUCCESS)
            return false;
        int best_score = -1;
        for (VkPhysicalDevice gpu : gpus) {
            uint32_t extension_count = 0;
            vk_.EnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, nullptr);
            std::vector<VkExtensionProperties> extensions(extension_count);
            vk_.EnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count,
                                                   extensions.data());
            if (!HasExtension(extensions, kVkSwapchainExtension))
                continue;
            uint32_t family_count = 0;
            vk_.GetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vk_.GetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
            for (uint32_t family = 0; family < family_count; ++family) {
                VkBool32 presents = kVkFalse;
                vk_.GetPhysicalDeviceSurfaceSupportKHR(gpu, family, surface_, &presents);
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || !presents)
                    continue;
                VkPhysicalDeviceProperties properties = {};
                vk_.GetPhysicalDeviceProperties(gpu, &properties);
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                                  : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                      ? 1
                                      : 0;
                if (score > best_score) {
                    best_score = score;
                    gpu_ = gpu;
                    queue_family_ = family;
                }
                break;
            }
        }
        if (gpu_ == nullptr)
            return false;
        vk_.GetPhysicalDeviceMemoryProperties(gpu_, &memory_);

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char* device_extensions[] = { kVkSwapchainExtension };
        VkDeviceCreateInfo device_info = {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        if (vk_.CreateDevice(gpu_, &device_info, nullptr, &device_) != VK_SUCCESS ||
            !vk_.LoadDevice(device_))
            return false;
        vk_.GetDeviceQueue(device_, queue_family_, 0, &queue_);

        // The swap chain's format, chosen once: plain 8-bit BGRA or RGBA - the same
        // non-sRGB-encoding target the Direct3D engines draw into - and failing both, the first
        // the surface offers. The chain targets use it too, so every pipeline suits both passes.
        uint32_t format_count = 0;
        vk_.GetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vk_.GetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &format_count, formats.data());
        if (format_count == 0)
            return false;
        format_ = formats[0].format;
        color_space_ = formats[0].colorSpace;
        for (const VkFormat wanted : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM }) {
            bool found = false;
            for (const VkSurfaceFormatKHR& format : formats) {
                if (format.format == wanted &&
                    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    format_ = format.format;
                    color_space_ = format.colorSpace;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }

        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        if (vk_.CreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS)
            return false;
        VkCommandBufferAllocateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buffer_info.commandPool = command_pool_;
        buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffer_info.commandBufferCount = 1;
        if (vk_.AllocateCommandBuffers(device_, &buffer_info, &commands_) != VK_SUCCESS)
            return false;
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // the first frame has nothing to wait for
        VkSemaphoreCreateInfo semaphore_info = {};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        return vk_.CreateFence(device_, &fence_info, nullptr, &frame_done_) == VK_SUCCESS &&
               vk_.CreateSemaphore(device_, &semaphore_info, nullptr, &acquired_) == VK_SUCCESS;
    }

    bool VulkanGraphicsEngine::CreateRenderPasses() {
        VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;

        // Into the window: cleared, so the letterbox bars are black, and left ready to present.
        VkAttachmentDescription attachment = {};
        attachment.format = format_;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        // The image is written only once the acquire has signalled, which the submit waits for at
        // this same stage.
        VkSubpassDependency acquire = {};
        acquire.srcSubpass = kVkSubpassExternal;
        acquire.dstSubpass = 0;
        acquire.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        acquire.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        acquire.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass_info = {};
        pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass_info.attachmentCount = 1;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;
        pass_info.dependencyCount = 1;
        pass_info.pDependencies = &acquire;
        if (vk_.CreateRenderPass(device_, &pass_info, nullptr, &window_pass_) != VK_SUCCESS)
            return false;

        // Into a chain target: every pixel is drawn, so nothing is loaded, and it is left ready
        // for the next pass to sample - which waits for it to be written.
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = kVkSubpassExternal;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = kVkSubpassExternal;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        pass_info.dependencyCount = 2;
        pass_info.pDependencies = dependencies;
        return vk_.CreateRenderPass(device_, &pass_info, nullptr, &target_pass_) == VK_SUCCESS;
    }

    bool VulkanGraphicsEngine::CreatePipelineObjects() {
        // D3D12GraphicsEngine's s0-s3. Its s0 and s1 wrap (CD3DX12_STATIC_SAMPLER_DESC's default),
        // which a filter reading past the edge of the picture sees.
        const VkFilter filters[4] = { VK_FILTER_NEAREST, VK_FILTER_LINEAR, VK_FILTER_NEAREST,
                                      VK_FILTER_LINEAR };
        const VkSamplerAddressMode modes[4] = {
            VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        for (int i = 0; i < 4; ++i) {
            VkSamplerCreateInfo sampler = {};
            sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler.magFilter = filters[i];
            sampler.minFilter = filters[i];
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = modes[i];
            sampler.addressModeV = modes[i];
            sampler.addressModeW = modes[i];
            sampler.maxAnisotropy = 1.0f;
            sampler.compareOp = VK_COMPARE_OP_NEVER;
            sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            if (vk_.CreateSampler(device_, &sampler, nullptr, &samplers_[i]) != VK_SUCCESS)
                return false;
        }

        VkDescriptorSetLayoutBinding bindings[kBindings] = {};
        for (uint32_t i = 0; i < kBindings; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = {};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = kBindings;
        set_info.pBindings = bindings;
        if (vk_.CreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_) != VK_SUCCESS)
            return false;

        VkPushConstantRange push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes };
        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (vk_.CreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) !=
            VK_SUCCESS)
            return false;

        VkShaderModuleCreateInfo module_info = {};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = kVertexSpirv.words * sizeof(uint32_t);
        module_info.pCode = kVertexSpirv.code;
        if (vk_.CreateShaderModule(device_, &module_info, nullptr, &vertex_) != VK_SUCCESS)
            return false;
        return CreateShader(kDefaultSpirv.code, kDefaultSpirv.words * sizeof(uint32_t),
                            &default_) &&
               CreateShader(kBlitSpirv.code, kBlitSpirv.words * sizeof(uint32_t), &blit_);
    }

    // One pipeline per fragment shader, made against the window's pass. The chain targets' pass
    // has the same one attachment in the same format, so Vulkan counts the two as compatible and
    // the same pipeline draws into either.
    bool VulkanGraphicsEngine::CreateShader(const uint32_t* code, size_t bytes, Shader* shader) {
        VkShaderModuleCreateInfo module_info = {};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = bytes;
        module_info.pCode = code;
        if (vk_.CreateShaderModule(device_, &module_info, nullptr, &shader->module) != VK_SUCCESS)
            return false;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = shader->module;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input = {};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly = {};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport = {};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster = {};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment = {};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_RGBA_BITS;
        VkPipelineColorBlendStateCreateInfo blend = {};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.logicOp = VK_LOGIC_OP_COPY;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkGraphicsPipelineCreateInfo pipeline = {};
        pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount = 2;
        pipeline.pStages = stages;
        pipeline.pVertexInputState = &vertex_input;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &multisample;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = pipeline_layout_;
        pipeline.renderPass = window_pass_;
        pipeline.basePipelineIndex = -1;
        if (vk_.CreateGraphicsPipelines(device_, nullptr, 1, &pipeline, nullptr,
                                        &shader->pipeline) != VK_SUCCESS) {
            DestroyShader(shader);
            return false;
        }
        return true;
    }

    void VulkanGraphicsEngine::DestroyShader(Shader* shader) {
        if (shader->pipeline != nullptr)
            vk_.DestroyPipeline(device_, shader->pipeline, nullptr);
        if (shader->module != nullptr)
            vk_.DestroyShaderModule(device_, shader->module, nullptr);
        *shader = Shader();
    }

    // ---------------------------------------------------------------------------------------------
    // The swap chain
    // ---------------------------------------------------------------------------------------------

    bool VulkanGraphicsEngine::CreateSwapchain() {
        VkSurfaceCapabilitiesKHR caps = {};
        if (vk_.GetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps) != VK_SUCCESS)
            return false;
        // The window's size, unless the surface insists on its own. Zero while minimised: no swap
        // chain then, and frames are skipped until it has a size again.
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == ~0u) {
            extent.width = static_cast<uint32_t>(width_);
            extent.height = static_cast<uint32_t>(height_);
        }
        if (extent.width == 0 || extent.height == 0)
            return true;

        uint32_t mode_count = 0;
        vk_.GetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vk_.GetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mode_count, modes.data());
        auto offers = [&modes](VkPresentModeKHR mode) {
            for (const VkPresentModeKHR offered : modes) {
                if (offered == mode)
                    return true;
            }
            return false;
        };
        // FIFO is vsync and always there. Without vsync, mailbox if the driver has it (no tearing,
        // no waiting), then immediate.
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync_) {
            if (offers(VK_PRESENT_MODE_MAILBOX_KHR))
                mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (offers(VK_PRESENT_MODE_IMMEDIATE_KHR))
                mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }

        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount != 0 && image_count > caps.maxImageCount)
            image_count = caps.maxImageCount;

        VkSwapchainCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = image_count;
        info.imageFormat = format_;
        info.imageColorSpace = color_space_;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
            for (VkFlags bit = 1; bit != 0; bit <<= 1) {
                if (caps.supportedCompositeAlpha & bit) {
                    info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                    break;
                }
            }
        }
        info.presentMode = mode;
        info.clipped = kVkTrue;
        if (vk_.CreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS)
            return false;
        extent_ = extent;

        uint32_t count = 0;
        vk_.GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        swap_images_.resize(count);
        vk_.GetSwapchainImagesKHR(device_, swapchain_, &count, swap_images_.data());
        swap_views_.assign(count, nullptr);
        swap_framebuffers_.assign(count, nullptr);
        rendered_.assign(count, nullptr);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageViewCreateInfo view = {};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = swap_images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format_;
            view.subresourceRange = ColorRange();
            if (vk_.CreateImageView(device_, &view, nullptr, &swap_views_[i]) != VK_SUCCESS)
                return false;
            VkFramebufferCreateInfo framebuffer = {};
            framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer.renderPass = window_pass_;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &swap_views_[i];
            framebuffer.width = extent.width;
            framebuffer.height = extent.height;
            framebuffer.layers = 1;
            if (vk_.CreateFramebuffer(device_, &framebuffer, nullptr, &swap_framebuffers_[i]) !=
                VK_SUCCESS)
                return false;
            // One per image: presenting waits on it, and it cannot be signalled again until that
            // image comes back from the acquire.
            VkSemaphoreCreateInfo semaphore = {};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vk_.CreateSemaphore(device_, &semaphore, nullptr, &rendered_[i]) != VK_SUCCESS)
                return false;
        }
        swapchain_dirty_ = false;
        return true;
    }

    void VulkanGraphicsEngine::DestroySwapchain() {
        if (device_ == nullptr)
            return;
        for (VkFramebuffer framebuffer : swap_framebuffers_) {
            if (framebuffer != nullptr)
                vk_.DestroyFramebuffer(device_, framebuffer, nullptr);
        }
        for (VkImageView view : swap_views_) {
            if (view != nullptr)
                vk_.DestroyImageView(device_, view, nullptr);
        }
        for (VkSemaphore semaphore : rendered_) {
            if (semaphore != nullptr)
                vk_.DestroySemaphore(device_, semaphore, nullptr);
        }
        swap_framebuffers_.clear();
        swap_views_.clear();
        rendered_.clear();
        swap_images_.clear();
        if (swapchain_ != nullptr)
            vk_.DestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = nullptr;
        extent_ = {};
    }

    // ---------------------------------------------------------------------------------------------
    // Shaders and filters
    // ---------------------------------------------------------------------------------------------

    bool VulkanGraphicsEngine::LoadCustomPixelShader(const std::string& name,
                                                     const uint8_t* bytecode, size_t size) {
        if (device_ == nullptr || name.empty() || bytecode == nullptr || size == 0 ||
            size % sizeof(uint32_t) != 0)
            return false;
        Shader shader;
        if (!CreateShader(reinterpret_cast<const uint32_t*>(bytecode), size, &shader))
            return false;
        auto existing = shaders_.find(name);
        if (existing != shaders_.end()) {
            vk_.DeviceWaitIdle(device_);   // the old one may be in the frame still drawing
            if (current_ == &existing->second)
                current_ = &default_;
            DestroyShader(&existing->second);
        }
        shaders_[name] = shader;
        return true;
    }

    bool VulkanGraphicsEngine::LoadShaderChain(const std::string& name,
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
        if (device_ != nullptr)
            vk_.DeviceWaitIdle(device_);
        const auto existing = chains_.find(name);
        if (existing != chains_.end() && active_chain_ == &existing->second)
            active_chain_ = nullptr;
        ReleaseChainTargets();
        chains_[name] = chain;
        return true;
    }

    void VulkanGraphicsEngine::SetPixelShader(const std::string& name) {
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

    void VulkanGraphicsEngine::SetVsync(bool enabled) {
        if (enabled != vsync_)
            swapchain_dirty_ = true;   // the present mode is the swap chain's
        vsync_ = enabled;
    }

    void VulkanGraphicsEngine::Resize(int width, int height) {
        if (width == width_ && height == height_)
            return;
        width_ = width;
        height_ = height;
        swapchain_dirty_ = true;
    }

    // ---------------------------------------------------------------------------------------------
    // Images
    // ---------------------------------------------------------------------------------------------

    int VulkanGraphicsEngine::FindMemory(uint32_t type_bits, VkFlags properties) const {
        for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) != 0 &&
                (memory_.memoryTypes[i].propertyFlags & properties) == properties)
                return static_cast<int>(i);
        }
        return -1;
    }

    bool VulkanGraphicsEngine::CreateImage(int width, int height, VkFormat format, VkFlags usage,
                                           Image* image) {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.CreateImage(device_, &info, nullptr, &image->image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements needs = {};
        vk_.GetImageMemoryRequirements(device_, image->image, &needs);
        const int type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = static_cast<uint32_t>(type);
        if (type < 0 ||
            vk_.AllocateMemory(device_, &allocate, nullptr, &image->memory) != VK_SUCCESS ||
            vk_.BindImageMemory(device_, image->image, image->memory, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo view = {};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = ColorRange();
        if (vk_.CreateImageView(device_, &view, nullptr, &image->view) != VK_SUCCESS)
            return false;
        image->width = width;
        image->height = height;
        return true;
    }

    void VulkanGraphicsEngine::DestroyImage(Image* image) {
        if (device_ != nullptr) {
            if (image->framebuffer != nullptr)
                vk_.DestroyFramebuffer(device_, image->framebuffer, nullptr);
            if (image->view != nullptr)
                vk_.DestroyImageView(device_, image->view, nullptr);
            if (image->image != nullptr)
                vk_.DestroyImage(device_, image->image, nullptr);
            if (image->memory != nullptr)
                vk_.FreeMemory(device_, image->memory, nullptr);
        }
        *image = Image();
    }

    // A descriptor set with `input` behind all four samplers and the frame on binding 4.
    VkDescriptorSet VulkanGraphicsEngine::AllocateReads(VkDescriptorPool pool, VkImageView input) {
        VkDescriptorSetAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &set_layout_;
        VkDescriptorSet set = nullptr;
        if (vk_.AllocateDescriptorSets(device_, &allocate, &set) != VK_SUCCESS)
            return nullptr;
        VkDescriptorImageInfo images[kBindings] = {};
        VkWriteDescriptorSet writes[kBindings] = {};
        for (uint32_t i = 0; i < kBindings; ++i) {
            const bool original = i == 4;
            images[i].sampler = samplers_[original ? 2 : i];
            images[i].imageView = original ? frame_.view : input;
            images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vk_.UpdateDescriptorSets(device_, kBindings, writes, 0, nullptr);
        return set;
    }

    // The frame's texture and its upload buffer, remade when the frame changes size - the PSX
    // changes resolution mid-boot. Only ever called after the last frame's fence, so nothing the
    // GPU is using goes.
    bool VulkanGraphicsEngine::EnsureFrameTexture(int width, int height) {
        if (frame_.image != nullptr && frame_.width == width && frame_.height == height)
            return true;
        ReleaseChainTargets();   // their sets read the frame too
        ReleaseFrameTexture();
        // BGRA bytes, which is how Gpu::ResolveFramebuffer packs a pixel - no swizzle anywhere.
        if (!CreateImage(width, height, VK_FORMAT_B8G8R8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &frame_))
            return false;
        frame_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        VkBufferCreateInfo buffer = {};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = static_cast<VkDeviceSize>(width) * height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vk_.CreateBuffer(device_, &buffer, nullptr, &staging_) != VK_SUCCESS)
            return false;
        VkMemoryRequirements needs = {};
        vk_.GetBufferMemoryRequirements(device_, staging_, &needs);
        const int type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = static_cast<uint32_t>(type);
        if (type < 0 ||
            vk_.AllocateMemory(device_, &allocate, nullptr, &staging_memory_) != VK_SUCCESS ||
            vk_.BindBufferMemory(device_, staging_, staging_memory_, 0) != VK_SUCCESS ||
            vk_.MapMemory(device_, staging_memory_, 0, buffer.size, 0, &staging_mapped_) !=
                VK_SUCCESS)
            return false;

        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBindings };
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vk_.CreateDescriptorPool(device_, &pool, nullptr, &frame_pool_) != VK_SUCCESS)
            return false;
        frame_.reads = AllocateReads(frame_pool_, frame_.view);
        return frame_.reads != nullptr;
    }

    void VulkanGraphicsEngine::ReleaseFrameTexture() {
        if (device_ != nullptr) {
            if (frame_pool_ != nullptr)
                vk_.DestroyDescriptorPool(device_, frame_pool_, nullptr);
            if (staging_mapped_ != nullptr)
                vk_.UnmapMemory(device_, staging_memory_);
            if (staging_ != nullptr)
                vk_.DestroyBuffer(device_, staging_, nullptr);
            if (staging_memory_ != nullptr)
                vk_.FreeMemory(device_, staging_memory_, nullptr);
        }
        frame_pool_ = nullptr;
        staging_mapped_ = nullptr;
        staging_ = nullptr;
        staging_memory_ = nullptr;
        DestroyImage(&frame_);
        frame_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // One texture per pass that renders at a multiple of the frame - of the frame, not of the pass
    // before (ShaderPass), so Super-xBR's three are all twice the frame.
    bool VulkanGraphicsEngine::EnsureChainTargets(const Chain& chain, int width, int height) {
        if (targets_for_ == &chain && targets_width_ == width && targets_height_ == height)
            return true;
        ReleaseChainTargets();
        size_t count = 0;
        for (const int scale : chain.scales)
            count += scale > 0 ? 1 : 0;
        if (count == 0)
            return false;
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      static_cast<uint32_t>(kBindings * count) };
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = static_cast<uint32_t>(count);
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vk_.CreateDescriptorPool(device_, &pool, nullptr, &chain_pool_) != VK_SUCCESS)
            return false;
        targets_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            Image& target = targets_[i];
            const int scale = chain.scales[i];
            bool made = CreateImage(width * scale, height * scale, format_,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT,
                                    &target);
            if (made) {
                VkFramebufferCreateInfo framebuffer = {};
                framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebuffer.renderPass = target_pass_;
                framebuffer.attachmentCount = 1;
                framebuffer.pAttachments = &target.view;
                framebuffer.width = static_cast<uint32_t>(target.width);
                framebuffer.height = static_cast<uint32_t>(target.height);
                framebuffer.layers = 1;
                made = vk_.CreateFramebuffer(device_, &framebuffer, nullptr,
                                             &target.framebuffer) == VK_SUCCESS;
            }
            if (made) {
                target.reads = AllocateReads(chain_pool_, target.view);
                made = target.reads != nullptr;
            }
            if (!made) {
                ReleaseChainTargets();
                return false;
            }
        }
        targets_for_ = &chain;
        targets_width_ = width;
        targets_height_ = height;
        return true;
    }

    void VulkanGraphicsEngine::ReleaseChainTargets() {
        for (Image& target : targets_)
            DestroyImage(&target);
        targets_.clear();
        if (device_ != nullptr && chain_pool_ != nullptr)
            vk_.DestroyDescriptorPool(device_, chain_pool_, nullptr);
        chain_pool_ = nullptr;
        targets_for_ = nullptr;
        targets_width_ = targets_height_ = 0;
    }

    // ---------------------------------------------------------------------------------------------
    // Frames
    // ---------------------------------------------------------------------------------------------

    void VulkanGraphicsEngine::Draw(VkCommandBuffer commands, const Shader& shader,
                                    VkDescriptorSet reads, float out_width, float out_height,
                                    float in_width, float in_height) {
        vk_.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shader.pipeline);
        vk_.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                  1, &reads, 0, nullptr);
        const float params[4] = { out_width, out_height, in_width, in_height };
        vk_.CmdPushConstants(commands, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             kPushBytes, params);
        vk_.CmdDraw(commands, 3, 1, 0, 0);
    }

    void VulkanGraphicsEngine::RenderFramebuffer(const void* data, int width, int height) {
        if (device_ == nullptr || data == nullptr || width <= 0 || height <= 0)
            return;
        // The last frame has to be finished before its command buffer, its upload buffer or
        // anything it drew with can be touched - and before the swap chain can be rebuilt.
        vk_.WaitForFences(device_, 1, &frame_done_, kVkTrue, kForever);
        if (swapchain_dirty_) {
            vk_.DeviceWaitIdle(device_);
            DestroySwapchain();
            if (!CreateSwapchain())
                return;
        }
        if (swapchain_ == nullptr || !EnsureFrameTexture(width, height))
            return;

        uint32_t image = 0;
        const VkResult acquired =
            vk_.AcquireNextImageKHR(device_, swapchain_, kForever, acquired_, nullptr, &image);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_dirty_ = true;
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return;

        memcpy(staging_mapped_, data, static_cast<size_t>(width) * height * 4);
        vk_.ResetCommandBuffer(commands_, 0);
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.BeginCommandBuffer(commands_, &begin);

        // Upload: the texture out of the shaders' way, the copy, and back.
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
        barrier.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
        barrier.image = frame_.image;
        barrier.subresourceRange = ColorRange();
        barrier.oldLayout = frame_layout_;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk_.CmdPipelineBarrier(commands_,
                               frame_layout_ == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                   : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &barrier);
        VkBufferImageCopy copy = {};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
        vk_.CmdCopyBufferToImage(commands_, staging_, frame_.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk_.CmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &barrier);
        frame_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // A fixed 4:3, as the other engines draw it (bug 47), placed exactly as D3D12 places it:
        // the letterbox as a fractional viewport, cut to whole pixels by the same scissor.
        const LetterboxRect rect =
            ComputeLetterboxRect(static_cast<int>(extent_.width), static_cast<int>(extent_.height),
                                 4.0f / 3.0f);
        const VkViewport window_view = { rect.x, rect.y, rect.width, rect.height, 0.0f, 1.0f };
        const int32_t left = static_cast<int32_t>(rect.x);
        const int32_t top = static_cast<int32_t>(rect.y);
        const VkRect2D window_scissor = {
            { left, top },
            { static_cast<uint32_t>(static_cast<int32_t>(rect.x + rect.width) - left),
              static_cast<uint32_t>(static_cast<int32_t>(rect.y + rect.height) - top) } };

        // A chain's passes into their own textures first; whatever draws last goes into the
        // window, as with a single shader. If the targets cannot be made, the frame is drawn
        // plain instead, as D3D12 does.
        const Shader* last = current_ != nullptr ? current_ : &default_;
        VkDescriptorSet last_reads = frame_.reads;
        float in_width = static_cast<float>(width);
        float in_height = static_cast<float>(height);
        if (active_chain_ != nullptr && EnsureChainTargets(*active_chain_, width, height)) {
            last = &blit_;
            for (size_t i = 0; i < active_chain_->passes.size(); ++i) {
                const Shader& pass = shaders_[active_chain_->passes[i]];
                if (active_chain_->scales[i] == 0) {
                    last = &pass;   // the last pass draws into the window itself
                    break;
                }
                const Image& target = targets_[i];
                VkRenderPassBeginInfo pass_begin = {};
                pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                pass_begin.renderPass = target_pass_;
                pass_begin.framebuffer = target.framebuffer;
                pass_begin.renderArea.extent = { static_cast<uint32_t>(target.width),
                                                 static_cast<uint32_t>(target.height) };
                vk_.CmdBeginRenderPass(commands_, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
                const VkViewport view = { 0.0f, 0.0f, static_cast<float>(target.width),
                                          static_cast<float>(target.height), 0.0f, 1.0f };
                vk_.CmdSetViewport(commands_, 0, 1, &view);
                vk_.CmdSetScissor(commands_, 0, 1, &pass_begin.renderArea);
                Draw(commands_, pass, last_reads, view.width, view.height, in_width, in_height);
                vk_.CmdEndRenderPass(commands_);
                last_reads = target.reads;
                in_width = view.width;
                in_height = view.height;
            }
        }

        VkClearValue clear = {};
        clear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo pass_begin = {};
        pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass_begin.renderPass = window_pass_;
        pass_begin.framebuffer = swap_framebuffers_[image];
        pass_begin.renderArea.extent = extent_;
        pass_begin.clearValueCount = 1;
        pass_begin.pClearValues = &clear;
        vk_.CmdBeginRenderPass(commands_, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vk_.CmdSetViewport(commands_, 0, 1, &window_view);
        vk_.CmdSetScissor(commands_, 0, 1, &window_scissor);
        // outW/outH are the letterbox's size, not the window's - Sharp Bilinear works out its
        // texel scale from them.
        Draw(commands_, *last, last_reads, rect.width, rect.height, in_width, in_height);
        vk_.CmdEndRenderPass(commands_);
        vk_.EndCommandBuffer(commands_);

        const VkFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered_[image];
        vk_.ResetFences(device_, 1, &frame_done_);
        if (vk_.QueueSubmit(queue_, 1, &submit, frame_done_) != VK_SUCCESS)
            return;
        pending_image_ = image;
        pending_present_ = true;
    }

    void VulkanGraphicsEngine::EndFrame() {
        if (!pending_present_)
            return;
        pending_present_ = false;
        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered_[pending_image_];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &pending_image_;
        const VkResult result = vk_.QueuePresentKHR(queue_, &present);
        // The window changed under it - resized, or moved to another monitor - and the next
        // frame rebuilds the swap chain.
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            swapchain_dirty_ = true;
    }

    // ---------------------------------------------------------------------------------------------
    // Taking it down
    // ---------------------------------------------------------------------------------------------

    void VulkanGraphicsEngine::Shutdown() {
        if (device_ != nullptr) {
            vk_.DeviceWaitIdle(device_);
            ReleaseChainTargets();
            ReleaseFrameTexture();
            DestroySwapchain();
            for (auto& shader : shaders_)
                DestroyShader(&shader.second);
            DestroyShader(&default_);
            DestroyShader(&blit_);
            if (vertex_ != nullptr)
                vk_.DestroyShaderModule(device_, vertex_, nullptr);
            if (pipeline_layout_ != nullptr)
                vk_.DestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            if (set_layout_ != nullptr)
                vk_.DestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            for (VkSampler sampler : samplers_) {
                if (sampler != nullptr)
                    vk_.DestroySampler(device_, sampler, nullptr);
            }
            if (target_pass_ != nullptr)
                vk_.DestroyRenderPass(device_, target_pass_, nullptr);
            if (window_pass_ != nullptr)
                vk_.DestroyRenderPass(device_, window_pass_, nullptr);
            if (acquired_ != nullptr)
                vk_.DestroySemaphore(device_, acquired_, nullptr);
            if (frame_done_ != nullptr)
                vk_.DestroyFence(device_, frame_done_, nullptr);
            if (command_pool_ != nullptr)
                vk_.DestroyCommandPool(device_, command_pool_, nullptr);
            vk_.DestroyDevice(device_, nullptr);
        }
        if (instance_ != nullptr) {
            if (surface_ != nullptr)
                vk_.DestroySurfaceKHR(instance_, surface_, nullptr);
            vk_.DestroyInstance(instance_, nullptr);
        }
        shaders_.clear();
        chains_.clear();
        current_ = nullptr;
        active_chain_ = nullptr;
        vertex_ = nullptr;
        pipeline_layout_ = nullptr;
        set_layout_ = nullptr;
        for (VkSampler& sampler : samplers_)
            sampler = nullptr;
        target_pass_ = window_pass_ = nullptr;
        acquired_ = nullptr;
        frame_done_ = nullptr;
        command_pool_ = nullptr;
        commands_ = nullptr;
        pending_present_ = false;
        device_ = nullptr;
        queue_ = nullptr;
        gpu_ = nullptr;
        surface_ = nullptr;
        instance_ = nullptr;
        vk_.Unload();
        // Out of the way of whichever engine draws next.
        if (window_ != nullptr)
            ShowWindowAsync(window_, SW_HIDE);
    }

}   // namespace psxemu
