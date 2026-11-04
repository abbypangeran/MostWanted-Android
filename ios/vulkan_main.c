#define SDL_MAIN_USE_CALLBACKS
#include <MoltenVK/mvk_vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct AppState {
    SDL_Window *window;
    bool loader_loaded;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSwapchainKHR swapchain;
    VkImageView *views;
    VkFramebuffer *framebuffers;
    uint32_t image_count;
    VkRenderPass render_pass;
    VkCommandPool command_pool;
    VkSemaphore acquired;
    VkSemaphore rendered;
} AppState;

static AppState app;

static bool VkOk(VkResult result, const char *operation) {
    if (result == VK_SUCCESS) return true;
    SDL_Log("vulkan: %s failed: VkResult %d", operation, (int)result);
    return false;
}

static bool HasInstanceExtension(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(extensions[i].extensionName, name) == 0) return true;
    return false;
}

static bool HasDeviceExtension(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (!VkOk(vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL), "device extension count")) return false;
    VkExtensionProperties *extensions = SDL_calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return false;
    bool found = false;
    if (VkOk(vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions), "device extensions")) {
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(extensions[i].extensionName, name) == 0) found = true;
    }
    SDL_free(extensions);
    return found;
}

static bool SelectPhysicalDevice(void) {
    uint32_t count = 0;
    if (!VkOk(vkEnumeratePhysicalDevices(app.instance, &count, NULL), "physical device count") || !count) {
        SDL_Log("vulkan: no physical devices enumerated");
        return false;
    }
    VkPhysicalDevice *devices = SDL_calloc(count, sizeof(*devices));
    if (!devices) return false;
    if (!VkOk(vkEnumeratePhysicalDevices(app.instance, &count, devices), "physical devices")) {
        SDL_free(devices);
        return false;
    }

    bool selected = false;
    for (uint32_t d = 0; d < count && !selected; ++d) {
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceProperties(devices[d], &properties);
        vkGetPhysicalDeviceFeatures(devices[d], &features);
        SDL_Log("vulkan device: %s API %u.%u.%u vendor=%04x device=%04x", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
                VK_API_VERSION_PATCH(properties.apiVersion), properties.vendorID, properties.deviceID);

        uint32_t extension_count = 0;
        if (!VkOk(vkEnumerateDeviceExtensionProperties(devices[d], NULL, &extension_count, NULL),
                  "device extension count")) continue;
        VkExtensionProperties *extensions = SDL_calloc(extension_count ? extension_count : 1, sizeof(*extensions));
        if (!extensions) continue;
        if (!VkOk(vkEnumerateDeviceExtensionProperties(devices[d], NULL, &extension_count, extensions),
                  "device extensions")) {
            SDL_free(extensions);
            continue;
        }
        for (uint32_t i = 0; i < extension_count; ++i)
            SDL_Log("vulkan device extension: %s", extensions[i].extensionName);
        bool swapchain = HasInstanceExtension(extensions, extension_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        bool portability_subset = HasInstanceExtension(extensions, extension_count,
                                                        VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        SDL_free(extensions);

        bool dynamic_available = properties.apiVersion >= VK_API_VERSION_1_3 ||
                                 HasDeviceExtension(devices[d], VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        bool timeline_available = properties.apiVersion >= VK_API_VERSION_1_2 ||
                                 HasDeviceExtension(devices[d], VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        VkPhysicalDeviceFeatures2 features2 = {0};
        VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic = {0};
        VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline = {0};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        dynamic.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
        timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
        if (properties.apiVersion >= VK_API_VERSION_1_1) {
            if (dynamic_available) features2.pNext = &dynamic;
            if (timeline_available) {
                if (dynamic_available) dynamic.pNext = &timeline;
                else features2.pNext = &timeline;
            }
            vkGetPhysicalDeviceFeatures2(devices[d], &features2);
        }
        SDL_Log("vulkan features: anisotropy=%u wireframe=%u BC=%u depthClamp=%u dynamicRendering=%u timeline=%u",
                features.samplerAnisotropy, features.fillModeNonSolid, features.textureCompressionBC,
                features.depthClamp, dynamic.dynamicRendering, timeline.timelineSemaphore);

        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        const VkFormat depth_candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        for (uint32_t i = 0; i < SDL_arraysize(depth_candidates); ++i) {
            VkFormatProperties format_properties;
            vkGetPhysicalDeviceFormatProperties(devices[d], depth_candidates[i], &format_properties);
            VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((format_properties.optimalTilingFeatures & required) == required) {
                depth_format = depth_candidates[i];
                break;
            }
        }
        SDL_Log("vulkan Xbox renderer gates: api11=%s swapchain=%s sampledDepth=%s",
                properties.apiVersion >= VK_API_VERSION_1_1 ? "yes" : "no",
                swapchain ? "yes" : "no", depth_format != VK_FORMAT_UNDEFINED ? "yes" : "no");

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queue_count, NULL);
        VkQueueFamilyProperties *queues = SDL_calloc(queue_count ? queue_count : 1, sizeof(*queues));
        if (!queues) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; ++q) {
            VkBool32 present = VK_FALSE;
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], q, app.surface, &present) == VK_SUCCESS && present &&
                properties.apiVersion >= VK_API_VERSION_1_1 && swapchain && depth_format != VK_FORMAT_UNDEFINED) {
                app.physical_device = devices[d];
                app.queue_family = q;
                selected = true;
                SDL_Log("vulkan selected: %s queue=%u portability_subset=%s", properties.deviceName, q,
                        portability_subset ? "yes" : "no");
                break;
            }
        }
        SDL_free(queues);
    }
    SDL_free(devices);
    return selected;
}

static bool CreateInstanceAndSurface(void) {
    uint32_t available_count = 0;
    if (!VkOk(vkEnumerateInstanceExtensionProperties(NULL, &available_count, NULL), "instance extension count"))
        return false;
    VkExtensionProperties *available = SDL_calloc(available_count ? available_count : 1, sizeof(*available));
    if (!available) return false;
    if (!VkOk(vkEnumerateInstanceExtensionProperties(NULL, &available_count, available), "instance extensions")) {
        SDL_free(available);
        return false;
    }
    for (uint32_t i = 0; i < available_count; ++i)
        SDL_Log("vulkan instance extension: %s", available[i].extensionName);

    uint32_t sdl_count = 0;
    const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    if (!sdl_extensions) {
        SDL_Log("vulkan: SDL instance extension query failed: %s", SDL_GetError());
        SDL_free(available);
        return false;
    }
    bool portability = HasInstanceExtension(available, available_count,
                                             VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    uint32_t enabled_count = sdl_count + (portability ? 1u : 0u);
    const char **enabled = SDL_calloc(enabled_count ? enabled_count : 1, sizeof(*enabled));
    if (!enabled) {
        SDL_free(available);
        return false;
    }
    for (uint32_t i = 0; i < sdl_count; ++i) {
        if (!HasInstanceExtension(available, available_count, sdl_extensions[i])) {
            SDL_Log("vulkan: SDL-required extension is unavailable: %s", sdl_extensions[i]);
            SDL_free(enabled);
            SDL_free(available);
            return false;
        }
        enabled[i] = sdl_extensions[i];
        SDL_Log("vulkan enabled SDL extension: %s", enabled[i]);
    }
    if (portability) {
        enabled[sdl_count] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        SDL_Log("vulkan enabled portability enumeration");
    }
    SDL_free(available);

    VkApplicationInfo application = {0};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Most Wanted Vulkan Bootstrap";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = enabled_count;
    create.ppEnabledExtensionNames = enabled;
    bool created = VkOk(vkCreateInstance(&create, NULL, &app.instance), "vkCreateInstance");
    SDL_free(enabled);
    if (!created) return false;
    SDL_Log("vulkan instance created");
    PFN_vkCreateMetalSurfaceEXT metal_surface =
        (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(app.instance, "vkCreateMetalSurfaceEXT");
    if (!metal_surface) {
        SDL_Log("vulkan: VK_EXT_metal_surface entry point is unavailable");
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(app.window, app.instance, NULL, &app.surface)) {
        SDL_Log("vulkan: SDL surface creation failed: %s", SDL_GetError());
        return false;
    }
    SDL_Log("vulkan SDL surface created with VK_EXT_metal_surface path");
    return true;
}

static bool CreateDevice(void) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {0};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = app.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char *extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                 VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME};
    uint32_t extension_count = HasDeviceExtension(app.physical_device,
                                                   VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) ? 2u : 1u;
    VkDeviceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = extension_count;
    create.ppEnabledExtensionNames = extensions;
    if (!VkOk(vkCreateDevice(app.physical_device, &create, NULL, &app.device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(app.device, app.queue_family, 0, &app.queue);
    SDL_Log("vulkan logical device and graphics/presentation queue created");
    return true;
}

static VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    const VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (uint32_t i = 0; i < SDL_arraysize(choices); ++i)
        if (supported & choices[i]) return choices[i];
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

static bool CreateSwapchain(void) {
    VkSurfaceCapabilitiesKHR capabilities;
    if (!VkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app.physical_device, app.surface, &capabilities),
              "surface capabilities")) return false;
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        SDL_Log("vulkan: surface does not support color attachments");
        return false;
    }
    uint32_t format_count = 0;
    if (!VkOk(vkGetPhysicalDeviceSurfaceFormatsKHR(app.physical_device, app.surface, &format_count, NULL),
              "surface format count") || !format_count) return false;
    VkSurfaceFormatKHR *formats = SDL_calloc(format_count, sizeof(*formats));
    if (!formats) return false;
    if (!VkOk(vkGetPhysicalDeviceSurfaceFormatsKHR(app.physical_device, app.surface, &format_count, formats),
              "surface formats")) {
        SDL_free(formats);
        return false;
    }
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = formats[i];
    }
    SDL_free(formats);

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(app.window, &width, &height);
        extent.width = (uint32_t)SDL_clamp(width, capabilities.minImageExtent.width,
                                           capabilities.maxImageExtent.width);
        extent.height = (uint32_t)SDL_clamp(height, capabilities.minImageExtent.height,
                                            capabilities.maxImageExtent.height);
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    VkSwapchainCreateInfoKHR create = {0};
    create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create.surface = app.surface;
    create.minImageCount = image_count;
    create.imageFormat = format.format;
    create.imageColorSpace = format.colorSpace;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create.clipped = VK_TRUE;
    if (!VkOk(vkCreateSwapchainKHR(app.device, &create, NULL, &app.swapchain), "vkCreateSwapchainKHR"))
        return false;
    if (!VkOk(vkGetSwapchainImagesKHR(app.device, app.swapchain, &app.image_count, NULL),
              "swapchain image count") || !app.image_count) return false;
    VkImage *images = SDL_calloc(app.image_count, sizeof(*images));
    app.views = SDL_calloc(app.image_count, sizeof(*app.views));
    app.framebuffers = SDL_calloc(app.image_count, sizeof(*app.framebuffers));
    if (!images || !app.views || !app.framebuffers) {
        SDL_free(images);
        return false;
    }
    if (!VkOk(vkGetSwapchainImagesKHR(app.device, app.swapchain, &app.image_count, images), "swapchain images")) {
        SDL_free(images);
        return false;
    }
    SDL_Log("vulkan swapchain created: %ux%u format=%d images=%u", extent.width, extent.height,
            (int)format.format, app.image_count);
    for (uint32_t i = 0; i < app.image_count; ++i) {
        VkImageViewCreateInfo view = {0};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format.format;
        view.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!VkOk(vkCreateImageView(app.device, &view, NULL, &app.views[i]), "vkCreateImageView")) {
            SDL_free(images);
            return false;
        }
    }
    SDL_free(images);

    VkAttachmentDescription attachment = {0};
    attachment.format = format.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency acquire_dependency = {0};
    acquire_dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    acquire_dependency.dstSubpass = 0;
    acquire_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquire_dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquire_dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_pass = {0};
    render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass.attachmentCount = 1;
    render_pass.pAttachments = &attachment;
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    render_pass.dependencyCount = 1;
    render_pass.pDependencies = &acquire_dependency;
    if (!VkOk(vkCreateRenderPass(app.device, &render_pass, NULL, &app.render_pass), "vkCreateRenderPass"))
        return false;
    for (uint32_t i = 0; i < app.image_count; ++i) {
        VkFramebufferCreateInfo framebuffer = {0};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = app.render_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &app.views[i];
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        if (!VkOk(vkCreateFramebuffer(app.device, &framebuffer, NULL, &app.framebuffers[i]),
                  "vkCreateFramebuffer")) return false;
    }

    VkCommandPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = app.queue_family;
    if (!VkOk(vkCreateCommandPool(app.device, &pool, NULL, &app.command_pool), "vkCreateCommandPool")) return false;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocation = {0};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = app.command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (!VkOk(vkAllocateCommandBuffers(app.device, &allocation, &command), "vkAllocateCommandBuffers"))
        return false;
    VkSemaphoreCreateInfo semaphore = {0};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!VkOk(vkCreateSemaphore(app.device, &semaphore, NULL, &app.acquired), "acquire semaphore") ||
        !VkOk(vkCreateSemaphore(app.device, &semaphore, NULL, &app.rendered), "render semaphore")) return false;

    uint32_t image_index = 0;
    VkResult acquired = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX, app.acquired,
                                               VK_NULL_HANDLE, &image_index);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return VkOk(acquired, "vkAcquireNextImageKHR");
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!VkOk(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer")) return false;
    VkClearValue clear = {.color = {{0.08f, 0.28f, 0.72f, 1.0f}}};
    VkRenderPassBeginInfo pass = {0};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = app.render_pass;
    pass.framebuffer = app.framebuffers[image_index];
    pass.renderArea.extent = extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(command);
    if (!VkOk(vkEndCommandBuffer(command), "vkEndCommandBuffer")) return false;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &app.acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &app.rendered;
    if (!VkOk(vkQueueSubmit(app.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit")) return false;
    VkPresentInfoKHR present = {0};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &app.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &app.swapchain;
    present.pImageIndices = &image_index;
    VkResult result = vkQueuePresentKHR(app.queue, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return VkOk(result, "vkQueuePresentKHR");
    if (!VkOk(vkQueueWaitIdle(app.queue), "vkQueueWaitIdle")) return false;
    SDL_Log("vulkan frame presented: color=0.08,0.28,0.72");
    return true;
}

static void Cleanup(void) {
    if (app.device) vkDeviceWaitIdle(app.device);
    if (app.device) {
        if (app.acquired) vkDestroySemaphore(app.device, app.acquired, NULL);
        if (app.rendered) vkDestroySemaphore(app.device, app.rendered, NULL);
        for (uint32_t i = 0; i < app.image_count; ++i) {
            if (app.framebuffers && app.framebuffers[i]) vkDestroyFramebuffer(app.device, app.framebuffers[i], NULL);
            if (app.views && app.views[i]) vkDestroyImageView(app.device, app.views[i], NULL);
        }
        SDL_free(app.framebuffers);
        SDL_free(app.views);
        app.framebuffers = NULL;
        app.views = NULL;
        if (app.render_pass) vkDestroyRenderPass(app.device, app.render_pass, NULL);
        if (app.swapchain) vkDestroySwapchainKHR(app.device, app.swapchain, NULL);
        if (app.command_pool) vkDestroyCommandPool(app.device, app.command_pool, NULL);
    }
    if (app.instance && app.surface) SDL_Vulkan_DestroySurface(app.instance, app.surface, NULL);
    if (app.device) vkDestroyDevice(app.device, NULL);
    if (app.instance) vkDestroyInstance(app.instance, NULL);
    if (app.loader_loaded) SDL_Vulkan_UnloadLibrary();
    if (app.window) SDL_DestroyWindow(app.window);
    SDL_zero(app);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    *appstate = &app;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        SDL_Log("SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app.loader_loaded = true;
    app.window = SDL_CreateWindow("Most Wanted Vulkan Bootstrap", 960, 540,
                                  SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) {
        SDL_Log("SDL Vulkan window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_Log("SDL iOS Vulkan window initialized; driver=%s window=%u", SDL_GetCurrentVideoDriver(),
            SDL_GetWindowID(app.window));
    if (!CreateInstanceAndSurface() || !SelectPhysicalDevice() || !CreateDevice() || !CreateSwapchain())
        return SDL_APP_FAILURE;
    SDL_Log("iOS Vulkan bootstrap passed");
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    (void)appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
        SDL_Log("Vulkan app terminating");
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        SDL_Log("Vulkan lifecycle: entering background");
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        SDL_Log("Vulkan lifecycle: foreground");
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    (void)appstate;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)appstate;
    (void)result;
    Cleanup();
    SDL_Log("Vulkan app quit");
}
