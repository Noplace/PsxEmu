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

// The part of Vulkan the Vulkan engine uses, declared here rather than by the Khronos headers.
//
// The official headers are 1.3 MB, 25,000 lines of vulkan_core.h alone, for the ~80 functions and
// ~70 structures a presenter touches; this is those, and no more - the same trade gl_functions.h
// makes for OpenGL. Every structure, constant and signature is transcribed from the Khronos
// Vulkan headers (version 1.4.337, Apache-2.0) and was checked against them: a scratch build that
// includes both static_asserts every structure's size and every member's offset, and every
// constant's value. Anything added here wants the same check.
//
// Nothing is linked. vulkan-1.dll - the Vulkan loader, which every current GPU driver installs -
// is opened at run time (VkFunctions::Load), so a machine without it just cannot pick Vulkan.
//
// Names follow the Khronos ones so the code reads like any Vulkan code, inside this namespace.
// The Khronos macros (VK_TRUE, VK_SUBPASS_EXTERNAL and so on) are kVk* constants instead, so this
// and the real headers can be compiled together for that check.

#include "app/framework.h"

#include <cstddef>
#include <cstdint>

namespace psxemu {

#define PSXEMU_VK_HANDLE(name) typedef struct name##_T* name;

    PSXEMU_VK_HANDLE(VkInstance)
    PSXEMU_VK_HANDLE(VkPhysicalDevice)
    PSXEMU_VK_HANDLE(VkDevice)
    PSXEMU_VK_HANDLE(VkQueue)
    PSXEMU_VK_HANDLE(VkCommandBuffer)
    PSXEMU_VK_HANDLE(VkSurfaceKHR)
    PSXEMU_VK_HANDLE(VkSwapchainKHR)
    PSXEMU_VK_HANDLE(VkImage)
    PSXEMU_VK_HANDLE(VkImageView)
    PSXEMU_VK_HANDLE(VkSampler)
    PSXEMU_VK_HANDLE(VkDeviceMemory)
    PSXEMU_VK_HANDLE(VkBuffer)
    PSXEMU_VK_HANDLE(VkBufferView)
    PSXEMU_VK_HANDLE(VkCommandPool)
    PSXEMU_VK_HANDLE(VkFence)
    PSXEMU_VK_HANDLE(VkSemaphore)
    PSXEMU_VK_HANDLE(VkRenderPass)
    PSXEMU_VK_HANDLE(VkFramebuffer)
    PSXEMU_VK_HANDLE(VkPipeline)
    PSXEMU_VK_HANDLE(VkPipelineCache)
    PSXEMU_VK_HANDLE(VkPipelineLayout)
    PSXEMU_VK_HANDLE(VkDescriptorSetLayout)
    PSXEMU_VK_HANDLE(VkDescriptorPool)
    PSXEMU_VK_HANDLE(VkDescriptorSet)
    PSXEMU_VK_HANDLE(VkShaderModule)
#undef PSXEMU_VK_HANDLE

    typedef uint32_t VkFlags;
    typedef uint32_t VkBool32;
    typedef uint64_t VkDeviceSize;
    typedef uint32_t VkSampleMask;

    inline constexpr VkBool32 kVkTrue = 1;
    inline constexpr VkBool32 kVkFalse = 0;
    inline constexpr uint32_t kVkSubpassExternal = ~0u;
    inline constexpr uint32_t kVkQueueFamilyIgnored = ~0u;
    inline constexpr uint32_t kVkApiVersion10 = 1u << 22;   // VK_MAKE_API_VERSION(0, 1, 0, 0)
    inline constexpr uint32_t kVkMaxMemoryTypes = 32;
    inline constexpr uint32_t kVkMaxMemoryHeaps = 16;
    inline constexpr uint32_t kVkMaxExtensionNameSize = 256;
    inline constexpr uint32_t kVkMaxPhysicalDeviceNameSize = 256;
    inline constexpr uint32_t kVkUuidSize = 16;
    inline constexpr const char kVkSurfaceExtension[] = "VK_KHR_surface";
    inline constexpr const char kVkWin32SurfaceExtension[] = "VK_KHR_win32_surface";
    inline constexpr const char kVkSwapchainExtension[] = "VK_KHR_swapchain";

    // -------------------------------------------------------------------------------------------
    // Enumerations - only the values used
    // -------------------------------------------------------------------------------------------

    enum VkResult : int32_t {
        VK_SUCCESS = 0,
        VK_NOT_READY = 1,
        VK_TIMEOUT = 2,
        VK_ERROR_DEVICE_LOST = -4,
        VK_ERROR_SURFACE_LOST_KHR = -1000000000,
        VK_SUBOPTIMAL_KHR = 1000001003,
        VK_ERROR_OUT_OF_DATE_KHR = -1000001004,
    };

    enum VkStructureType : int32_t {
        VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
        VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000,
    };

    enum VkFormat : int32_t {
        VK_FORMAT_UNDEFINED = 0,
        VK_FORMAT_R8G8B8A8_UNORM = 37,
        VK_FORMAT_B8G8R8A8_UNORM = 44,
        VK_FORMAT_R32G32_SFLOAT = 103,   // the overlay's positions and texture coordinates
    };

    // The overlay's vertices and indices (ui/overlay) - the only drawing here that has either.
    enum VkVertexInputRate : int32_t { VK_VERTEX_INPUT_RATE_VERTEX = 0 };
    enum VkIndexType : int32_t { VK_INDEX_TYPE_UINT32 = 1 };

    enum VkColorSpaceKHR : int32_t { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0 };

    enum VkPresentModeKHR : int32_t {
        VK_PRESENT_MODE_IMMEDIATE_KHR = 0,
        VK_PRESENT_MODE_MAILBOX_KHR = 1,
        VK_PRESENT_MODE_FIFO_KHR = 2,
    };

    enum VkPhysicalDeviceType : int32_t {
        VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
        VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    };

    enum VkSharingMode : int32_t { VK_SHARING_MODE_EXCLUSIVE = 0 };
    enum VkImageType : int32_t { VK_IMAGE_TYPE_2D = 1 };
    enum VkImageTiling : int32_t { VK_IMAGE_TILING_OPTIMAL = 0 };
    enum VkImageViewType : int32_t { VK_IMAGE_VIEW_TYPE_2D = 1 };
    enum VkComponentSwizzle : int32_t { VK_COMPONENT_SWIZZLE_IDENTITY = 0 };

    enum VkImageLayout : int32_t {
        VK_IMAGE_LAYOUT_UNDEFINED = 0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002,
    };

    enum VkFilter : int32_t { VK_FILTER_NEAREST = 0, VK_FILTER_LINEAR = 1 };
    enum VkSamplerMipmapMode : int32_t { VK_SAMPLER_MIPMAP_MODE_NEAREST = 0 };
    enum VkSamplerAddressMode : int32_t {
        VK_SAMPLER_ADDRESS_MODE_REPEAT = 0,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2,
    };
    enum VkCompareOp : int32_t { VK_COMPARE_OP_NEVER = 0 };
    enum VkBorderColor : int32_t { VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK = 0 };

    enum VkAttachmentLoadOp : int32_t {
        VK_ATTACHMENT_LOAD_OP_LOAD = 0,
        VK_ATTACHMENT_LOAD_OP_CLEAR = 1,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
    };
    enum VkAttachmentStoreOp : int32_t {
        VK_ATTACHMENT_STORE_OP_STORE = 0,
        VK_ATTACHMENT_STORE_OP_DONT_CARE = 1,
    };

    enum VkPipelineBindPoint : int32_t { VK_PIPELINE_BIND_POINT_GRAPHICS = 0 };
    enum VkDescriptorType : int32_t { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1 };
    enum VkPrimitiveTopology : int32_t { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3 };
    enum VkPolygonMode : int32_t { VK_POLYGON_MODE_FILL = 0 };
    enum VkFrontFace : int32_t { VK_FRONT_FACE_COUNTER_CLOCKWISE = 0 };
    enum VkBlendFactor : int32_t {
        VK_BLEND_FACTOR_ZERO = 0,
        VK_BLEND_FACTOR_ONE = 1,
        VK_BLEND_FACTOR_SRC_ALPHA = 6,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7,
    };
    enum VkBlendOp : int32_t { VK_BLEND_OP_ADD = 0 };
    enum VkLogicOp : int32_t { VK_LOGIC_OP_COPY = 3 };
    enum VkDynamicState : int32_t { VK_DYNAMIC_STATE_VIEWPORT = 0, VK_DYNAMIC_STATE_SCISSOR = 1 };
    enum VkCommandBufferLevel : int32_t { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 };
    enum VkSubpassContents : int32_t { VK_SUBPASS_CONTENTS_INLINE = 0 };
    enum VkSampleCountFlagBits : int32_t { VK_SAMPLE_COUNT_1_BIT = 1 };
    enum VkSurfaceTransformFlagBitsKHR : int32_t { VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 1 };
    enum VkCompositeAlphaFlagBitsKHR : int32_t { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 1 };
    enum VkShaderStageFlagBits : int32_t {
        VK_SHADER_STAGE_VERTEX_BIT = 0x01,
        VK_SHADER_STAGE_FRAGMENT_BIT = 0x10,
    };

    // Flag bits, as plain constants: they are OR'd into VkFlags fields.
    inline constexpr VkFlags VK_QUEUE_GRAPHICS_BIT = 0x1;
    inline constexpr VkFlags VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1;
    inline constexpr VkFlags VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2;
    inline constexpr VkFlags VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4;
    inline constexpr VkFlags VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2;
    inline constexpr VkFlags VK_IMAGE_USAGE_SAMPLED_BIT = 0x4;
    inline constexpr VkFlags VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10;
    inline constexpr VkFlags VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x1;
    inline constexpr VkFlags VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 0x40;
    inline constexpr VkFlags VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x80;
    inline constexpr VkFlags VK_IMAGE_ASPECT_COLOR_BIT = 0x1;
    inline constexpr VkFlags VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1;
    inline constexpr VkFlags VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x80;
    inline constexpr VkFlags VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x400;
    inline constexpr VkFlags VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000;
    inline constexpr VkFlags VK_ACCESS_SHADER_READ_BIT = 0x20;
    inline constexpr VkFlags VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x100;
    inline constexpr VkFlags VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000;
    inline constexpr VkFlags VK_COLOR_COMPONENT_RGBA_BITS = 0xF;
    inline constexpr VkFlags VK_CULL_MODE_NONE = 0;
    inline constexpr VkFlags VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2;
    inline constexpr VkFlags VK_FENCE_CREATE_SIGNALED_BIT = 0x1;
    inline constexpr VkFlags VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1;

    // -------------------------------------------------------------------------------------------
    // Structures
    // -------------------------------------------------------------------------------------------

    struct VkApplicationInfo {
        VkStructureType sType;
        const void* pNext;
        const char* pApplicationName;
        uint32_t applicationVersion;
        const char* pEngineName;
        uint32_t engineVersion;
        uint32_t apiVersion;
    };

    struct VkInstanceCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        const VkApplicationInfo* pApplicationInfo;
        uint32_t enabledLayerCount;
        const char* const* ppEnabledLayerNames;
        uint32_t enabledExtensionCount;
        const char* const* ppEnabledExtensionNames;
    };

    struct VkWin32SurfaceCreateInfoKHR {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        HINSTANCE hinstance;
        HWND hwnd;
    };

    struct VkExtent2D { uint32_t width, height; };
    struct VkExtent3D { uint32_t width, height, depth; };
    struct VkOffset2D { int32_t x, y; };
    struct VkOffset3D { int32_t x, y, z; };
    struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };

    struct VkQueueFamilyProperties {
        VkFlags queueFlags;
        uint32_t queueCount;
        uint32_t timestampValidBits;
        VkExtent3D minImageTransferGranularity;
    };

    struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; };
    struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; };

    struct VkPhysicalDeviceMemoryProperties {
        uint32_t memoryTypeCount;
        VkMemoryType memoryTypes[kVkMaxMemoryTypes];
        uint32_t memoryHeapCount;
        VkMemoryHeap memoryHeaps[kVkMaxMemoryHeaps];
    };

    // Only the front of it is read; the limits and sparse properties behind are kept as bytes,
    // sized to match (the check asserts the whole size).
    struct VkPhysicalDeviceProperties {
        uint32_t apiVersion;
        uint32_t driverVersion;
        uint32_t vendorID;
        uint32_t deviceID;
        VkPhysicalDeviceType deviceType;
        char deviceName[kVkMaxPhysicalDeviceNameSize];
        uint8_t pipelineCacheUUID[kVkUuidSize];
        alignas(8) uint8_t limitsAndSparseProperties[528];
    };

    struct VkExtensionProperties {
        char extensionName[kVkMaxExtensionNameSize];
        uint32_t specVersion;
    };

    struct VkDeviceQueueCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t queueFamilyIndex;
        uint32_t queueCount;
        const float* pQueuePriorities;
    };

    struct VkDeviceCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t queueCreateInfoCount;
        const VkDeviceQueueCreateInfo* pQueueCreateInfos;
        uint32_t enabledLayerCount;
        const char* const* ppEnabledLayerNames;
        uint32_t enabledExtensionCount;
        const char* const* ppEnabledExtensionNames;
        const void* pEnabledFeatures;   // VkPhysicalDeviceFeatures; always null here
    };

    struct VkSurfaceCapabilitiesKHR {
        uint32_t minImageCount;
        uint32_t maxImageCount;
        VkExtent2D currentExtent;
        VkExtent2D minImageExtent;
        VkExtent2D maxImageExtent;
        uint32_t maxImageArrayLayers;
        VkFlags supportedTransforms;
        VkSurfaceTransformFlagBitsKHR currentTransform;
        VkFlags supportedCompositeAlpha;
        VkFlags supportedUsageFlags;
    };

    struct VkSurfaceFormatKHR { VkFormat format; VkColorSpaceKHR colorSpace; };

    struct VkSwapchainCreateInfoKHR {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkSurfaceKHR surface;
        uint32_t minImageCount;
        VkFormat imageFormat;
        VkColorSpaceKHR imageColorSpace;
        VkExtent2D imageExtent;
        uint32_t imageArrayLayers;
        VkFlags imageUsage;
        VkSharingMode imageSharingMode;
        uint32_t queueFamilyIndexCount;
        const uint32_t* pQueueFamilyIndices;
        VkSurfaceTransformFlagBitsKHR preTransform;
        VkCompositeAlphaFlagBitsKHR compositeAlpha;
        VkPresentModeKHR presentMode;
        VkBool32 clipped;
        VkSwapchainKHR oldSwapchain;
    };

    struct VkImageCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkImageType imageType;
        VkFormat format;
        VkExtent3D extent;
        uint32_t mipLevels;
        uint32_t arrayLayers;
        VkSampleCountFlagBits samples;
        VkImageTiling tiling;
        VkFlags usage;
        VkSharingMode sharingMode;
        uint32_t queueFamilyIndexCount;
        const uint32_t* pQueueFamilyIndices;
        VkImageLayout initialLayout;
    };

    struct VkMemoryRequirements {
        VkDeviceSize size;
        VkDeviceSize alignment;
        uint32_t memoryTypeBits;
    };

    struct VkMemoryAllocateInfo {
        VkStructureType sType;
        const void* pNext;
        VkDeviceSize allocationSize;
        uint32_t memoryTypeIndex;
    };

    struct VkBufferCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkDeviceSize size;
        VkFlags usage;
        VkSharingMode sharingMode;
        uint32_t queueFamilyIndexCount;
        const uint32_t* pQueueFamilyIndices;
    };

    struct VkComponentMapping { VkComponentSwizzle r, g, b, a; };

    struct VkImageSubresourceRange {
        VkFlags aspectMask;
        uint32_t baseMipLevel;
        uint32_t levelCount;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
    };

    struct VkImageViewCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkImage image;
        VkImageViewType viewType;
        VkFormat format;
        VkComponentMapping components;
        VkImageSubresourceRange subresourceRange;
    };

    struct VkSamplerCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkFilter magFilter;
        VkFilter minFilter;
        VkSamplerMipmapMode mipmapMode;
        VkSamplerAddressMode addressModeU;
        VkSamplerAddressMode addressModeV;
        VkSamplerAddressMode addressModeW;
        float mipLodBias;
        VkBool32 anisotropyEnable;
        float maxAnisotropy;
        VkBool32 compareEnable;
        VkCompareOp compareOp;
        float minLod;
        float maxLod;
        VkBorderColor borderColor;
        VkBool32 unnormalizedCoordinates;
    };

    struct VkAttachmentDescription {
        VkFlags flags;
        VkFormat format;
        VkSampleCountFlagBits samples;
        VkAttachmentLoadOp loadOp;
        VkAttachmentStoreOp storeOp;
        VkAttachmentLoadOp stencilLoadOp;
        VkAttachmentStoreOp stencilStoreOp;
        VkImageLayout initialLayout;
        VkImageLayout finalLayout;
    };

    struct VkAttachmentReference { uint32_t attachment; VkImageLayout layout; };

    struct VkSubpassDescription {
        VkFlags flags;
        VkPipelineBindPoint pipelineBindPoint;
        uint32_t inputAttachmentCount;
        const VkAttachmentReference* pInputAttachments;
        uint32_t colorAttachmentCount;
        const VkAttachmentReference* pColorAttachments;
        const VkAttachmentReference* pResolveAttachments;
        const VkAttachmentReference* pDepthStencilAttachment;
        uint32_t preserveAttachmentCount;
        const uint32_t* pPreserveAttachments;
    };

    struct VkSubpassDependency {
        uint32_t srcSubpass;
        uint32_t dstSubpass;
        VkFlags srcStageMask;
        VkFlags dstStageMask;
        VkFlags srcAccessMask;
        VkFlags dstAccessMask;
        VkFlags dependencyFlags;
    };

    struct VkRenderPassCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t attachmentCount;
        const VkAttachmentDescription* pAttachments;
        uint32_t subpassCount;
        const VkSubpassDescription* pSubpasses;
        uint32_t dependencyCount;
        const VkSubpassDependency* pDependencies;
    };

    struct VkFramebufferCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkRenderPass renderPass;
        uint32_t attachmentCount;
        const VkImageView* pAttachments;
        uint32_t width;
        uint32_t height;
        uint32_t layers;
    };

    struct VkShaderModuleCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        size_t codeSize;
        const uint32_t* pCode;
    };

    struct VkPushConstantRange { VkFlags stageFlags; uint32_t offset; uint32_t size; };

    struct VkPipelineLayoutCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t setLayoutCount;
        const VkDescriptorSetLayout* pSetLayouts;
        uint32_t pushConstantRangeCount;
        const VkPushConstantRange* pPushConstantRanges;
    };

    struct VkDescriptorSetLayoutBinding {
        uint32_t binding;
        VkDescriptorType descriptorType;
        uint32_t descriptorCount;
        VkFlags stageFlags;
        const VkSampler* pImmutableSamplers;
    };

    struct VkDescriptorSetLayoutCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t bindingCount;
        const VkDescriptorSetLayoutBinding* pBindings;
    };

    struct VkDescriptorPoolSize { VkDescriptorType type; uint32_t descriptorCount; };

    struct VkDescriptorPoolCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t maxSets;
        uint32_t poolSizeCount;
        const VkDescriptorPoolSize* pPoolSizes;
    };

    struct VkDescriptorSetAllocateInfo {
        VkStructureType sType;
        const void* pNext;
        VkDescriptorPool descriptorPool;
        uint32_t descriptorSetCount;
        const VkDescriptorSetLayout* pSetLayouts;
    };

    struct VkDescriptorImageInfo {
        VkSampler sampler;
        VkImageView imageView;
        VkImageLayout imageLayout;
    };

    struct VkWriteDescriptorSet {
        VkStructureType sType;
        const void* pNext;
        VkDescriptorSet dstSet;
        uint32_t dstBinding;
        uint32_t dstArrayElement;
        uint32_t descriptorCount;
        VkDescriptorType descriptorType;
        const VkDescriptorImageInfo* pImageInfo;
        const void* pBufferInfo;   // VkDescriptorBufferInfo; unused
        const VkBufferView* pTexelBufferView;
    };

    struct VkPipelineShaderStageCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkShaderStageFlagBits stage;
        VkShaderModule module;
        const char* pName;
        const void* pSpecializationInfo;   // VkSpecializationInfo; unused
    };

    struct VkVertexInputBindingDescription {
        uint32_t binding;
        uint32_t stride;
        VkVertexInputRate inputRate;
    };

    struct VkVertexInputAttributeDescription {
        uint32_t location;
        uint32_t binding;
        VkFormat format;
        uint32_t offset;
    };

    struct VkPipelineVertexInputStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t vertexBindingDescriptionCount;
        // None for the picture's passes, whose triangle comes from the vertex id; the overlay's
        // one binding otherwise.
        const VkVertexInputBindingDescription* pVertexBindingDescriptions;
        uint32_t vertexAttributeDescriptionCount;
        const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
    };

    struct VkPipelineInputAssemblyStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkPrimitiveTopology topology;
        VkBool32 primitiveRestartEnable;
    };

    struct VkViewport { float x, y, width, height, minDepth, maxDepth; };

    struct VkPipelineViewportStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t viewportCount;
        const VkViewport* pViewports;
        uint32_t scissorCount;
        const VkRect2D* pScissors;
    };

    struct VkPipelineRasterizationStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkBool32 depthClampEnable;
        VkBool32 rasterizerDiscardEnable;
        VkPolygonMode polygonMode;
        VkFlags cullMode;
        VkFrontFace frontFace;
        VkBool32 depthBiasEnable;
        float depthBiasConstantFactor;
        float depthBiasClamp;
        float depthBiasSlopeFactor;
        float lineWidth;
    };

    struct VkPipelineMultisampleStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkSampleCountFlagBits rasterizationSamples;
        VkBool32 sampleShadingEnable;
        float minSampleShading;
        const VkSampleMask* pSampleMask;
        VkBool32 alphaToCoverageEnable;
        VkBool32 alphaToOneEnable;
    };

    struct VkPipelineColorBlendAttachmentState {
        VkBool32 blendEnable;
        VkBlendFactor srcColorBlendFactor;
        VkBlendFactor dstColorBlendFactor;
        VkBlendOp colorBlendOp;
        VkBlendFactor srcAlphaBlendFactor;
        VkBlendFactor dstAlphaBlendFactor;
        VkBlendOp alphaBlendOp;
        VkFlags colorWriteMask;
    };

    struct VkPipelineColorBlendStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        VkBool32 logicOpEnable;
        VkLogicOp logicOp;
        uint32_t attachmentCount;
        const VkPipelineColorBlendAttachmentState* pAttachments;
        float blendConstants[4];
    };

    struct VkPipelineDynamicStateCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t dynamicStateCount;
        const VkDynamicState* pDynamicStates;
    };

    struct VkGraphicsPipelineCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t stageCount;
        const VkPipelineShaderStageCreateInfo* pStages;
        const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
        const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
        const void* pTessellationState;
        const VkPipelineViewportStateCreateInfo* pViewportState;
        const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
        const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
        const void* pDepthStencilState;
        const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
        const VkPipelineDynamicStateCreateInfo* pDynamicState;
        VkPipelineLayout layout;
        VkRenderPass renderPass;
        uint32_t subpass;
        VkPipeline basePipelineHandle;
        int32_t basePipelineIndex;
    };

    struct VkCommandPoolCreateInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        uint32_t queueFamilyIndex;
    };

    struct VkCommandBufferAllocateInfo {
        VkStructureType sType;
        const void* pNext;
        VkCommandPool commandPool;
        VkCommandBufferLevel level;
        uint32_t commandBufferCount;
    };

    struct VkCommandBufferBeginInfo {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        const void* pInheritanceInfo;
    };

    struct VkFenceCreateInfo { VkStructureType sType; const void* pNext; VkFlags flags; };
    struct VkSemaphoreCreateInfo { VkStructureType sType; const void* pNext; VkFlags flags; };

    struct VkImageMemoryBarrier {
        VkStructureType sType;
        const void* pNext;
        VkFlags srcAccessMask;
        VkFlags dstAccessMask;
        VkImageLayout oldLayout;
        VkImageLayout newLayout;
        uint32_t srcQueueFamilyIndex;
        uint32_t dstQueueFamilyIndex;
        VkImage image;
        VkImageSubresourceRange subresourceRange;
    };

    struct VkImageSubresourceLayers {
        VkFlags aspectMask;
        uint32_t mipLevel;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
    };

    struct VkBufferImageCopy {
        VkDeviceSize bufferOffset;
        uint32_t bufferRowLength;
        uint32_t bufferImageHeight;
        VkImageSubresourceLayers imageSubresource;
        VkOffset3D imageOffset;
        VkExtent3D imageExtent;
    };

    union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; };
    struct VkClearDepthStencilValue { float depth; uint32_t stencil; };
    union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; };

    struct VkRenderPassBeginInfo {
        VkStructureType sType;
        const void* pNext;
        VkRenderPass renderPass;
        VkFramebuffer framebuffer;
        VkRect2D renderArea;
        uint32_t clearValueCount;
        const VkClearValue* pClearValues;
    };

    struct VkSubmitInfo {
        VkStructureType sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const VkSemaphore* pWaitSemaphores;
        const VkFlags* pWaitDstStageMask;
        uint32_t commandBufferCount;
        const VkCommandBuffer* pCommandBuffers;
        uint32_t signalSemaphoreCount;
        const VkSemaphore* pSignalSemaphores;
    };

    struct VkPresentInfoKHR {
        VkStructureType sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const VkSemaphore* pWaitSemaphores;
        uint32_t swapchainCount;
        const VkSwapchainKHR* pSwapchains;
        const uint32_t* pImageIndices;
        VkResult* pResults;
    };

    // -------------------------------------------------------------------------------------------
    // Functions
    // -------------------------------------------------------------------------------------------

    typedef void(__stdcall* PFN_vkVoidFunction)(void);
    typedef PFN_vkVoidFunction(__stdcall* PFN_vkGetInstanceProcAddr)(VkInstance instance,
                                                                     const char* name);

    // Every entry: return type, name without "vk", parameters. The allocator argument every
    // create and destroy takes is always null here, so it is a plain pointer.
#define PSXEMU_VK_INSTANCE_FUNCTIONS(X)                                                            \
    X(void, DestroyInstance, (VkInstance, const void*))                                            \
    X(VkResult, EnumeratePhysicalDevices, (VkInstance, uint32_t*, VkPhysicalDevice*))              \
    X(void, GetPhysicalDeviceProperties, (VkPhysicalDevice, VkPhysicalDeviceProperties*))          \
    X(void, GetPhysicalDeviceQueueFamilyProperties,                                                \
      (VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*))                                     \
    X(void, GetPhysicalDeviceMemoryProperties,                                                     \
      (VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*))                                       \
    X(VkResult, EnumerateDeviceExtensionProperties,                                                \
      (VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*))                          \
    X(VkResult, CreateWin32SurfaceKHR,                                                             \
      (VkInstance, const VkWin32SurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*))                \
    X(void, DestroySurfaceKHR, (VkInstance, VkSurfaceKHR, const void*))                            \
    X(VkResult, GetPhysicalDeviceSurfaceSupportKHR,                                                \
      (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*))                                       \
    X(VkResult, GetPhysicalDeviceSurfaceCapabilitiesKHR,                                           \
      (VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*))                                 \
    X(VkResult, GetPhysicalDeviceSurfaceFormatsKHR,                                                \
      (VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*))                            \
    X(VkResult, GetPhysicalDeviceSurfacePresentModesKHR,                                           \
      (VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*))                              \
    X(VkResult, CreateDevice, (VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*)) \
    X(PFN_vkVoidFunction, GetDeviceProcAddr, (VkDevice, const char*))

#define PSXEMU_VK_DEVICE_FUNCTIONS(X)                                                              \
    X(void, DestroyDevice, (VkDevice, const void*))                                                \
    X(void, GetDeviceQueue, (VkDevice, uint32_t, uint32_t, VkQueue*))                              \
    X(VkResult, DeviceWaitIdle, (VkDevice))                                                        \
    X(VkResult, CreateSwapchainKHR,                                                                \
      (VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*))                   \
    X(void, DestroySwapchainKHR, (VkDevice, VkSwapchainKHR, const void*))                          \
    X(VkResult, GetSwapchainImagesKHR, (VkDevice, VkSwapchainKHR, uint32_t*, VkImage*))            \
    X(VkResult, AcquireNextImageKHR,                                                               \
      (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*))                       \
    X(VkResult, QueuePresentKHR, (VkQueue, const VkPresentInfoKHR*))                               \
    X(VkResult, QueueSubmit, (VkQueue, uint32_t, const VkSubmitInfo*, VkFence))                    \
    X(VkResult, CreateImage, (VkDevice, const VkImageCreateInfo*, const void*, VkImage*))          \
    X(void, DestroyImage, (VkDevice, VkImage, const void*))                                        \
    X(void, GetImageMemoryRequirements, (VkDevice, VkImage, VkMemoryRequirements*))                \
    X(VkResult, AllocateMemory,                                                                    \
      (VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*))                       \
    X(void, FreeMemory, (VkDevice, VkDeviceMemory, const void*))                                   \
    X(VkResult, BindImageMemory, (VkDevice, VkImage, VkDeviceMemory, VkDeviceSize))                \
    X(VkResult, CreateBuffer, (VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*))       \
    X(void, DestroyBuffer, (VkDevice, VkBuffer, const void*))                                      \
    X(void, GetBufferMemoryRequirements, (VkDevice, VkBuffer, VkMemoryRequirements*))              \
    X(VkResult, BindBufferMemory, (VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize))              \
    X(VkResult, MapMemory, (VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)) \
    X(void, UnmapMemory, (VkDevice, VkDeviceMemory))                                               \
    X(VkResult, CreateImageView,                                                                   \
      (VkDevice, const VkImageViewCreateInfo*, const void*, VkImageView*))                         \
    X(void, DestroyImageView, (VkDevice, VkImageView, const void*))                                \
    X(VkResult, CreateSampler, (VkDevice, const VkSamplerCreateInfo*, const void*, VkSampler*))    \
    X(void, DestroySampler, (VkDevice, VkSampler, const void*))                                    \
    X(VkResult, CreateRenderPass,                                                                  \
      (VkDevice, const VkRenderPassCreateInfo*, const void*, VkRenderPass*))                       \
    X(void, DestroyRenderPass, (VkDevice, VkRenderPass, const void*))                              \
    X(VkResult, CreateFramebuffer,                                                                 \
      (VkDevice, const VkFramebufferCreateInfo*, const void*, VkFramebuffer*))                     \
    X(void, DestroyFramebuffer, (VkDevice, VkFramebuffer, const void*))                            \
    X(VkResult, CreateShaderModule,                                                                \
      (VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*))                   \
    X(void, DestroyShaderModule, (VkDevice, VkShaderModule, const void*))                          \
    X(VkResult, CreatePipelineLayout,                                                              \
      (VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*))               \
    X(void, DestroyPipelineLayout, (VkDevice, VkPipelineLayout, const void*))                      \
    X(VkResult, CreateGraphicsPipelines, (VkDevice, VkPipelineCache, uint32_t,                     \
                                          const VkGraphicsPipelineCreateInfo*, const void*,        \
                                          VkPipeline*))                                            \
    X(void, DestroyPipeline, (VkDevice, VkPipeline, const void*))                                  \
    X(VkResult, CreateDescriptorSetLayout,                                                         \
      (VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*))     \
    X(void, DestroyDescriptorSetLayout, (VkDevice, VkDescriptorSetLayout, const void*))            \
    X(VkResult, CreateDescriptorPool,                                                              \
      (VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*))               \
    X(void, DestroyDescriptorPool, (VkDevice, VkDescriptorPool, const void*))                      \
    X(VkResult, AllocateDescriptorSets,                                                            \
      (VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*))                            \
    X(void, UpdateDescriptorSets,                                                                  \
      (VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*))                    \
    X(VkResult, CreateCommandPool,                                                                 \
      (VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*))                     \
    X(void, DestroyCommandPool, (VkDevice, VkCommandPool, const void*))                            \
    X(VkResult, AllocateCommandBuffers,                                                            \
      (VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*))                            \
    X(VkResult, BeginCommandBuffer, (VkCommandBuffer, const VkCommandBufferBeginInfo*))            \
    X(VkResult, EndCommandBuffer, (VkCommandBuffer))                                               \
    X(VkResult, ResetCommandBuffer, (VkCommandBuffer, VkFlags))                                    \
    X(VkResult, CreateFence, (VkDevice, const VkFenceCreateInfo*, const void*, VkFence*))          \
    X(void, DestroyFence, (VkDevice, VkFence, const void*))                                        \
    X(VkResult, WaitForFences, (VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t))           \
    X(VkResult, ResetFences, (VkDevice, uint32_t, const VkFence*))                                 \
    X(VkResult, CreateSemaphore,                                                                   \
      (VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*))                         \
    X(void, DestroySemaphore, (VkDevice, VkSemaphore, const void*))                                \
    X(void, CmdPipelineBarrier, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t,             \
                                 const void*, uint32_t, const void*, uint32_t,                     \
                                 const VkImageMemoryBarrier*))                                     \
    X(void, CmdCopyBufferToImage, (VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t,    \
                                   const VkBufferImageCopy*))                                      \
    X(void, CmdBeginRenderPass,                                                                    \
      (VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents))                          \
    X(void, CmdEndRenderPass, (VkCommandBuffer))                                                   \
    X(void, CmdBindPipeline, (VkCommandBuffer, VkPipelineBindPoint, VkPipeline))                   \
    X(void, CmdBindDescriptorSets, (VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout,        \
                                    uint32_t, uint32_t, const VkDescriptorSet*, uint32_t,          \
                                    const uint32_t*))                                              \
    X(void, CmdPushConstants,                                                                      \
      (VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*))               \
    X(void, CmdSetViewport, (VkCommandBuffer, uint32_t, uint32_t, const VkViewport*))              \
    X(void, CmdSetScissor, (VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*))                 \
    X(void, CmdDraw, (VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t))                    \
    X(void, CmdBindVertexBuffers,                                                                  \
      (VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*))                 \
    X(void, CmdBindIndexBuffer, (VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType))            \
    X(void, CmdDrawIndexed, (VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t))

    struct VkFunctions {
        typedef VkResult(__stdcall* CreateInstanceProc)(const VkInstanceCreateInfo*, const void*,
                                                        VkInstance*);
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
        CreateInstanceProc CreateInstance = nullptr;

#define PSXEMU_VK_DECLARE(ret, name, params)                                                       \
    typedef ret(__stdcall* name##Proc) params;                                                     \
    name##Proc name = nullptr;
        PSXEMU_VK_INSTANCE_FUNCTIONS(PSXEMU_VK_DECLARE)
        PSXEMU_VK_DEVICE_FUNCTIONS(PSXEMU_VK_DECLARE)
#undef PSXEMU_VK_DECLARE

        HMODULE library = nullptr;

        // Opens the Vulkan loader. False on a machine without one.
        bool LoadLibraryAndGlobals() {
            library = LoadLibraryW(L"vulkan-1.dll");
            if (library == nullptr)
                return false;
            GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr")));
            if (GetInstanceProcAddr == nullptr)
                return false;
            CreateInstance = reinterpret_cast<CreateInstanceProc>(
                GetInstanceProcAddr(nullptr, "vkCreateInstance"));
            return CreateInstance != nullptr;
        }

        bool LoadInstance(VkInstance instance) {
            bool all = true;
#define PSXEMU_VK_LOAD(ret, name, params)                                                          \
    name = reinterpret_cast<name##Proc>(GetInstanceProcAddr(instance, "vk" #name));                \
    all = all && name != nullptr;
            PSXEMU_VK_INSTANCE_FUNCTIONS(PSXEMU_VK_LOAD)
#undef PSXEMU_VK_LOAD
            return all;
        }

        bool LoadDevice(VkDevice device) {
            bool all = true;
#define PSXEMU_VK_LOAD(ret, name, params)                                                          \
    name = reinterpret_cast<name##Proc>(GetDeviceProcAddr(device, "vk" #name));                    \
    all = all && name != nullptr;
            PSXEMU_VK_DEVICE_FUNCTIONS(PSXEMU_VK_LOAD)
#undef PSXEMU_VK_LOAD
            return all;
        }

        void Unload() {
            if (library != nullptr)
                FreeLibrary(library);
            *this = VkFunctions();
        }
    };

}   // namespace psxemu
