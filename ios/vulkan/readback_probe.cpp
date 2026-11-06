#include <MoltenVK/mvk_vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#include "kernel/checked_gpu_transfer.h"

namespace {

constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 4;
constexpr std::size_t kSourcePitch = 8 * 4;
constexpr std::size_t kSourceBytes = kSourcePitch * kHeight;
constexpr uint32_t kGuestPitch = 64;
constexpr std::size_t kGuestBytes = kGuestPitch * kHeight;

bool VkOk(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "VULKAN_READBACK_FAIL: %s VkResult=%d\n", operation, int(result));
    return false;
}

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t candidates, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, bool* has_preferred = nullptr) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t pass = 0; pass != 2; ++pass) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if (!(candidates & (1u << i))) continue;
            const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required) continue;
            const bool matches = (flags & preferred) == preferred;
            if ((pass == 0) == matches) {
                if (has_preferred) *has_preferred = matches;
                return i;
            }
        }
    }
    return UINT32_MAX;
}

std::size_t CopyReadback(void* context, uint8_t* destination, std::size_t capacity) {
    const auto* source = static_cast<const uint8_t*>(context);
    if (capacity < kSourceBytes) return 0;
    std::memcpy(destination, source, kSourceBytes);
    return kSourceBytes;
}

struct Probe {
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    uint32_t guest_buffer;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize allocation_size = 0;
    bool coherent = false;

    ~Probe() {
        if (device) vkDeviceWaitIdle(device);
        if (mapped && buffer_memory) vkUnmapMemory(device, buffer_memory);
        if (command) vkFreeCommandBuffers(device, pool, 1, &command);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (image_memory) vkFreeMemory(device, image_memory, nullptr);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (buffer_memory) vkFreeMemory(device, buffer_memory, nullptr);
    }

    bool Run() {
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format);
        constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((format.optimalTilingFeatures & needed) != needed) {
            std::fprintf(stderr, "VULKAN_READBACK_FAIL: RGBA8 color attachment/readback unsupported\n");
            return false;
        }

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {kWidth, kHeight, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!VkOk(vkCreateImage(device, &image_info, nullptr, &image), "create offscreen image")) return false;

        VkMemoryRequirements image_requirements{};
        vkGetImageMemoryRequirements(device, image, &image_requirements);
        const uint32_t image_type = FindMemoryType(physical, image_requirements.memoryTypeBits, 0,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (image_type == UINT32_MAX) return false;
        VkMemoryAllocateInfo image_allocation{};
        image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_allocation.allocationSize = image_requirements.size;
        image_allocation.memoryTypeIndex = image_type;
        if (!VkOk(vkAllocateMemory(device, &image_allocation, nullptr, &image_memory), "allocate image memory") ||
            !VkOk(vkBindImageMemory(device, image, image_memory, 0), "bind image memory")) return false;

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!VkOk(vkCreateImageView(device, &view_info, nullptr, &view), "create image view")) return false;

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        const VkSubpassDependency dependencies[] = {
            {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
            {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0}};
        VkRenderPassCreateInfo pass_info{};
        pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass_info.attachmentCount = 1;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;
        pass_info.dependencyCount = 2;
        pass_info.pDependencies = dependencies;
        if (!VkOk(vkCreateRenderPass(device, &pass_info, nullptr, &render_pass), "create readback render pass"))
            return false;
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &view;
        framebuffer_info.width = kWidth;
        framebuffer_info.height = kHeight;
        framebuffer_info.layers = 1;
        if (!VkOk(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer), "create framebuffer"))
            return false;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = kSourceBytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!VkOk(vkCreateBuffer(device, &buffer_info, nullptr, &buffer), "create staging buffer")) return false;
        VkMemoryRequirements buffer_requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
        const uint32_t buffer_type = FindMemoryType(physical, buffer_requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &coherent);
        if (buffer_type == UINT32_MAX) return false;
        allocation_size = buffer_requirements.size;
        VkMemoryAllocateInfo buffer_allocation{};
        buffer_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_allocation.allocationSize = allocation_size;
        buffer_allocation.memoryTypeIndex = buffer_type;
        if (!VkOk(vkAllocateMemory(device, &buffer_allocation, nullptr, &buffer_memory), "allocate staging memory") ||
            !VkOk(vkBindBufferMemory(device, buffer, buffer_memory, 0), "bind staging memory") ||
            !VkOk(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map staging memory")) return false;
        std::memset(mapped, 0xD7, kSourceBytes);
        if (!coherent) {
            VkMappedMemoryRange flush{};
            flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            flush.memory = buffer_memory;
            flush.size = VK_WHOLE_SIZE;
            if (!VkOk(vkFlushMappedMemoryRanges(device, 1, &flush), "flush staging memory")) return false;
        }

        VkCommandBufferAllocateInfo command_allocation{};
        command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocation.commandPool = pool;
        command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocation.commandBufferCount = 1;
        if (!VkOk(vkAllocateCommandBuffers(device, &command_allocation, &command), "allocate readback command"))
            return false;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!VkOk(vkCreateFence(device, &fence_info, nullptr, &fence), "create readback fence")) return false;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!VkOk(vkBeginCommandBuffer(command, &begin), "begin readback command")) return false;
        VkRenderPassBeginInfo render{};
        render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render.renderPass = render_pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent = {kWidth, kHeight};
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        VkClearRect rect{};
        rect.rect.extent = {kWidth, kHeight / 2};
        rect.layerCount = 1;
        clear.clearValue.color.float32[0] = 1.0f;
        clear.clearValue.color.float32[3] = 1.0f;
        vkCmdClearAttachments(command, 1, &clear, 1, &rect);
        rect.rect.offset.y = kHeight / 2;
        clear.clearValue.color.float32[0] = 0.0f;
        clear.clearValue.color.float32[2] = 1.0f;
        vkCmdClearAttachments(command, 1, &clear, 1, &rect);
        vkCmdEndRenderPass(command);
        VkBufferImageCopy copy{};
        copy.bufferRowLength = 8;
        copy.bufferImageHeight = kHeight;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        if (!VkOk(vkEndCommandBuffer(command), "end readback command")) return false;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        if (!VkOk(vkQueueSubmit(queue, 1, &submit, fence), "submit readback") ||
            !VkOk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for readback fence")) return false;
        if (!coherent) {
            VkMappedMemoryRange invalidate{};
            invalidate.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            invalidate.memory = buffer_memory;
            invalidate.size = VK_WHOLE_SIZE;
            if (!VkOk(vkInvalidateMappedMemoryRanges(device, 1, &invalidate), "invalidate staging memory")) return false;
        }

        std::vector<uint8_t> initial(kGuestBytes, 0xA5);
        if (mem::Write(guest_buffer, initial.data(), initial.size()) != mem::AccessStatus::kOk) return false;
        std::vector<uint8_t> published;
        const auto result = kernel::checked_gpu_transfer::Transfer(
            guest_buffer, kGuestBytes, kWidth, kHeight, kSourcePitch, mapped, CopyReadback, published);
        std::vector<uint8_t> actual(kGuestBytes);
        std::vector<uint8_t> expected = initial;
        std::vector<uint8_t> expected_pixels(kWidth * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y) {
            const std::array<uint8_t, 4> pixel = y < kHeight / 2 ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                                                  : std::array<uint8_t, 4>{0, 0, 255, 255};
            for (uint32_t x = 0; x < kWidth; ++x) {
                const std::size_t at = (std::size_t(y) * kWidth + x) * 4;
                std::memcpy(expected_pixels.data() + at, pixel.data(), pixel.size());
                std::memcpy(expected.data() + std::size_t(y) * kGuestPitch + x * 4, pixel.data(), pixel.size());
                const auto* row = static_cast<const uint8_t*>(mapped) + std::size_t(y) * kSourcePitch;
                if (std::memcmp(row + x * 4, pixel.data(), pixel.size()) != 0) return false;
            }
            for (std::size_t x = kWidth * 4; x < kSourcePitch; ++x)
                if (static_cast<const uint8_t*>(mapped)[std::size_t(y) * kSourcePitch + x] != 0xD7) return false;
        }
        if (result.status != kernel::checked_gpu_transfer::Status::kOk ||
            mem::Read(guest_buffer, actual.data(), actual.size()) != mem::AccessStatus::kOk ||
            actual != expected || published != expected_pixels) return false;
        SDL_Log("VULKAN_READBACK_PASS: %ux%u RGBA8, source_pitch=%zu, guest_pitch=%u, fence-complete, padding-preserved",
                kWidth, kHeight, kSourcePitch, kGuestPitch);
        return true;
    }
};

}  // namespace

extern "C" int RunVulkanReadbackProbe(VkPhysicalDevice physical, VkDevice device, VkQueue queue, VkCommandPool pool) {
    const bool owns_memory = !mem::IsInitialized();
    if (owns_memory && !mem::Init()) return 0;
    uint32_t guest_buffer = 0;
    uint32_t guest_buffer_size = mem::kPage;
    const uint32_t allocated = mem::AllocateVirtual(guest_buffer, guest_buffer_size,
                                                     mem::kMemReserve | mem::kMemCommit, mem::kReadWrite);
    bool passed = false;
    if (!allocated) {
        try {
            Probe probe{physical, device, queue, pool, guest_buffer};
            passed = probe.Run();
        } catch (...) {
            SDL_Log("VULKAN_READBACK_FAIL: probe threw while creating or validating readback resources");
        }
        uint32_t release_base = guest_buffer;
        uint32_t release_size = guest_buffer_size;
        if (mem::FreeVirtual(release_base, release_size, mem::kMemRelease) != 0) {
            SDL_Log("VULKAN_READBACK_FAIL: could not release probe guest memory");
            passed = false;
        }
    } else {
        SDL_Log("VULKAN_READBACK_FAIL: could not allocate probe guest memory, status=%08X", allocated);
    }
    if (owns_memory) mem::Shutdown();
    if (!passed) SDL_Log("VULKAN_READBACK_FAIL: probe did not validate expected guest pixels");
    return passed ? 1 : 0;
}
