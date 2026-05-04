/**
 * @file SmokeTest.cpp
 * @brief Minimal NVK bring-up probe for Nintendo Switch homebrew.
 *
 * This example answers one question: can the Switch NVK build create a Vulkan
 * instance, find a device, and reach the VI surface/swapchain path?
 *
 * Lifecycle:
 * 1. Bring up the libnx console.
 * 2. Enumerate instance extensions.
 * 3. Create a VkInstance with VK_KHR_surface and VK_NN_vi_surface.
 * 4. Inspect the first physical device and its present-capable queue.
 * 5. Probe device creation, command submission, swapchain, acquire, clear,
 *    and present.
 * 6. Destroy Vulkan objects and wait for + before exit.
 *
 * Build with ./build.sh.
 */

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <vector>

#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>

extern "C" {
/**
 * Request application memory instead of the small applet heap.
 *
 * NVK channel initialization exceeds the applet pool during bring-up, so these
 * symbols ask libnx for the regular application memory profile.
 */
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;  // 0 = request all available
}
static bool g_console_active = false;
static bool g_deferred_vk_message_valid = false;
static uint32_t g_deferred_vk_message_count = 0;
static char g_deferred_vk_severity[16];
static char g_deferred_vk_message[512];

/** Return a stable short name for the VkResult values printed by examples. */
static const char *
vk_result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VK_ERROR_(other)";
    }
}

/** Return the present mode name used in smoke-test status output. */
static const char *
present_mode_str(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

/** Append one line to the SD-card trace file used for crash-side debugging. */
static void
trace(const char *msg)
{
    std::FILE *f = std::fopen("sdmc:/SmokeTest-trace.log", "a");
    if (f) {
        std::fprintf(f, "%s\n", msg);
        std::fclose(f);
    }
}

/**
 * Debug-utils callback that survives console handoff.
 *
 * Surface and scanout probes may temporarily release the libnx console. During
 * that window we keep the latest message and flush it after the console comes
 * back, instead of printing to a dead framebuffer.
 */
static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_utils_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
               VkDebugUtilsMessageTypeFlagsEXT types,
               const VkDebugUtilsMessengerCallbackDataEXT *data,
               void *user_data)
{
    const char *sev = "other";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        sev = "error";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        sev = "warn";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        sev = "info";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
        sev = "verbose";

    const char *message =
        data && data->pMessage ? data->pMessage : "(no message)";

    if (!g_console_active) {
        std::snprintf(g_deferred_vk_severity, sizeof(g_deferred_vk_severity),
                      "%s", sev);
        std::snprintf(g_deferred_vk_message, sizeof(g_deferred_vk_message),
                      "%s", message);
        g_deferred_vk_message_valid = true;
        g_deferred_vk_message_count++;
        return VK_FALSE;
    }

    std::printf("[vk-%s] %s\n", sev, message);
    if (g_console_active)
        consoleUpdate(nullptr);
    return VK_FALSE;
}

/** Refresh the console only while it is owned by this example. */
static void
refresh_console()
{
    if (g_console_active)
        consoleUpdate(nullptr);
}

/** Print the most recent debug-utils message deferred during console release. */
static void
flush_deferred_vk_messages()
{
    if (!g_console_active || !g_deferred_vk_message_valid)
        return;

    std::printf("[vk-%s] %s\n", g_deferred_vk_severity,
                g_deferred_vk_message);
    if (g_deferred_vk_message_count > 1) {
        std::printf("[vk-info] %u Vulkan debug messages occurred while "
                    "the console was inactive; showing the latest\n",
                    g_deferred_vk_message_count);
    }
    g_deferred_vk_message_valid = false;
    g_deferred_vk_message_count = 0;
    refresh_console();
}

/** Release the default NWindow before VI surface scanout uses it. */
static void
release_console()
{
    if (!g_console_active)
        return;

    std::fflush(stdout);
    consoleExit(nullptr);
    g_console_active = false;
}

/** Acquire the libnx console if the example currently owns no console. */
static void
init_console()
{
    if (g_console_active)
        return;

    consoleInit(nullptr);
    g_console_active = true;
}

/** Keep final logs visible until the user presses +. */
static void
wait_for_exit()
{
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    std::printf("\nPress + to exit.\n");
    refresh_console();

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(16000000ULL);
        refresh_console();
    }
}

/** Enumerate global instance extensions, returning an empty list on failure. */
static std::vector<VkExtensionProperties>
enumerate_instance_extensions()
{
    uint32_t count = 0;
    VkResult r =
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (r != VK_SUCCESS) {
        std::printf("[!] vkEnumerateInstanceExtensionProperties(count) -> %s\n",
                    vk_result_str(r));
        return {};
    }

    std::vector<VkExtensionProperties> props(count);
    if (count > 0) {
        r = vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                                   props.data());
        if (r != VK_SUCCESS) {
            std::printf("[!] vkEnumerateInstanceExtensionProperties(list) -> %s\n",
                        vk_result_str(r));
            return {};
        }
    }

    props.resize(count);
    return props;
}

/** Print instance extensions in the compact format used by all examples. */
static void
dump_instance_extensions(const std::vector<VkExtensionProperties> &props)
{
    std::printf("[i] %u instance extensions:\n",
                static_cast<unsigned>(props.size()));
    for (const auto &p : props)
        std::printf("    - %s (rev %u)\n", p.extensionName, p.specVersion);
}

/** True if an extension list contains @p name. */
static bool
has_extension(const std::vector<VkExtensionProperties> &props,
              const char *name)
{
    for (const auto &p : props) {
        if (std::strcmp(p.extensionName, name) == 0)
            return true;
    }

    return false;
}

/** Prefer FIFO because it is required by Vulkan and maps cleanly to display vsync. */
static VkPresentModeKHR
choose_present_mode(const std::vector<VkPresentModeKHR> &modes)
{
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            return mode;
    }

    return modes.empty() ? VK_PRESENT_MODE_FIFO_KHR : modes[0];
}

/** Pick the first supported composite-alpha mode that does not imply blending. */
static VkCompositeAlphaFlagBitsKHR
choose_composite_alpha(VkCompositeAlphaFlagsKHR supported)
{
    if (supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/** Instance handles owned by this smoke test. */
struct InstanceProbe {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
};

/** Create the Vulkan instance and optional debug-utils messenger. */
static bool
create_instance(const std::vector<const char *> &enabled_exts,
                bool enable_debug_utils,
                InstanceProbe *out)
{
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "nvk-smoke";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount =
        static_cast<uint32_t>(enabled_exts.size());
    ici.ppEnabledExtensionNames = enabled_exts.data();

    VkResult r = vkCreateInstance(&ici, nullptr, &out->instance);
    std::printf("[i] vkCreateInstance -> %s\n", vk_result_str(r));
    refresh_console();
    if (r != VK_SUCCESS)
        return false;

    if (!enable_debug_utils)
        return true;

    VkDebugUtilsMessengerCreateInfoEXT dmci = {};
    dmci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dmci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    dmci.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dmci.pfnUserCallback = debug_utils_cb;
    vkCreateDebugUtilsMessengerEXT(out->instance, &dmci, nullptr,
                                   &out->debug_messenger);
    return true;
}

/** Destroy instance-level Vulkan handles in reverse creation order. */
static void
destroy_instance(InstanceProbe *probe)
{
    trace("pre DestroyDebugMessenger");
    if (probe->debug_messenger) {
        vkDestroyDebugUtilsMessengerEXT(probe->instance,
                                        probe->debug_messenger, nullptr);
        probe->debug_messenger = VK_NULL_HANDLE;
    }

    trace("pre vkDestroyInstance");
    if (probe->instance) {
        vkDestroyInstance(probe->instance, nullptr);
        probe->instance = VK_NULL_HANDLE;
    }
}

/** Print physical devices and choose the first one for the probe. */
static bool
pick_physical_device(VkInstance instance, VkPhysicalDevice *out)
{
    uint32_t pd_count = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (r != VK_SUCCESS) {
        std::printf("[!] vkEnumeratePhysicalDevices(count) -> %s\n",
                    vk_result_str(r));
        return false;
    }
    std::printf("[i] %u physical device(s)\n", pd_count);

    std::vector<VkPhysicalDevice> pds(pd_count);
    if (pd_count) {
        r = vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
        if (r != VK_SUCCESS) {
            std::printf("[!] vkEnumeratePhysicalDevices(list) -> %s\n",
                        vk_result_str(r));
            return false;
        }
        pds.resize(pd_count);
    }

    *out = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < pd_count; i++) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(pds[i], &props);
        std::printf("    [%u] %s  api=%u.%u.%u  driver=0x%08x\n",
                    i, props.deviceName,
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion),
                    VK_API_VERSION_PATCH(props.apiVersion),
                    props.driverVersion);

        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qf_count, nullptr);
        std::printf("        %u queue families\n", qf_count);

        if (*out == VK_NULL_HANDLE)
            *out = pds[i];
    }

    refresh_console();
    return true;
}

/**
 * Run the smoke probe.
 *
 * @return 0 when the probe completed, 1 when required setup failed.
 */
int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    init_console();

    std::printf("=== NVK smoke test ===\n");
    refresh_console();

    const std::vector<VkExtensionProperties> instance_exts =
        enumerate_instance_extensions();
    const bool have_surface_ext =
        has_extension(instance_exts, VK_KHR_SURFACE_EXTENSION_NAME);
    const bool have_vi_surface_ext =
        has_extension(instance_exts, VK_NN_VI_SURFACE_EXTENSION_NAME);
    const bool have_debug_utils_ext =
        has_extension(instance_exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    dump_instance_extensions(instance_exts);
    refresh_console();

    if (!have_surface_ext || !have_vi_surface_ext) {
        std::printf("[X] missing required instance extensions:%s%s\n",
                    have_surface_ext ? "" : " VK_KHR_surface",
                    have_vi_surface_ext ? "" : " VK_NN_vi_surface");
        wait_for_exit();
        release_console();
        return 1;
    }

    std::vector<const char *> enabled_exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_NN_VI_SURFACE_EXTENSION_NAME,
    };
    if (have_debug_utils_ext) {
        enabled_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        std::printf("[i] VK_EXT_debug_utils not advertised; continuing without it\n");
    }

    InstanceProbe instance_probe;
    if (!create_instance(enabled_exts, have_debug_utils_ext,
                         &instance_probe)) {
        wait_for_exit();
        release_console();
        return 1;
    }

    const VkInstance instance = instance_probe.instance;
    int exit_code = 0;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!pick_physical_device(instance, &pd))
        exit_code = 1;

    // Step 3: Try VK_NN_vi_surface against the default NWindow
    if (exit_code == 0 && pd != VK_NULL_HANDLE) {
        NWindow *nw = nwindowGetDefault();
        VkViSurfaceCreateInfoNN sci = {};
        sci.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
        sci.window = nw;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const uint32_t invalid_qf = ~0u;
        uint32_t present_qf_index = invalid_qf;
        bool has_swapchain_ext = false;
        VkSurfaceCapabilitiesKHR caps = {};
        std::vector<VkSurfaceFormatKHR> surface_formats;
        std::vector<VkPresentModeKHR> present_modes;
        VkDevice device = VK_NULL_HANDLE;
        const bool run_present_only_probe = false;
        const bool run_visible_scanout_probe = true;
        bool swapchain_probe_attempted = false;
        VkResult swapchain_r = VK_INCOMPLETE;
        VkResult swapchain_images_r = VK_INCOMPLETE;
        VkResult swapchain_images_list_r = VK_INCOMPLETE;
        VkResult acquire_r = VK_INCOMPLETE;
        VkResult acquire_present_r = VK_INCOMPLETE;
        VkResult acquire_clear_r = VK_INCOMPLETE;
        VkResult clear_begin_r = VK_INCOMPLETE;
        VkResult clear_cmd_alloc_r = VK_INCOMPLETE;
        VkResult clear_cmd_end_r = VK_INCOMPLETE;
        VkResult clear_pool_r = VK_INCOMPLETE;
        VkResult clear_present_r = VK_INCOMPLETE;
        VkResult clear_submit_r = VK_INCOMPLETE;
        VkResult clear_wait_r = VK_INCOMPLETE;
        VkResult clear_queue_idle_r = VK_INCOMPLETE;
        VkResult cmd_begin_r = VK_INCOMPLETE;
        VkResult cmd_end_r = VK_INCOMPLETE;
        VkResult cmd_pool_r = VK_INCOMPLETE;
        VkResult cmd_alloc_r = VK_INCOMPLETE;
        VkResult submit_r = VK_INCOMPLETE;
        VkResult present_r = VK_INCOMPLETE;
        VkResult wait_r = VK_INCOMPLETE;
        VkResult wait_submit_r = VK_INCOMPLETE;
        VkExtent2D swapchain_extent = {};
        uint32_t swapchain_image_count = 0;
        uint32_t swapchain_image_query_count = 0;
        uint32_t swapchain_image_list_count = 0;
        uint32_t acquired_image_index = UINT32_MAX;
        uint32_t clear_image_index = UINT32_MAX;
        uint32_t presented_image_index = UINT32_MAX;
        VkPresentModeKHR swapchain_mode = VK_PRESENT_MODE_FIFO_KHR;
        std::vector<VkImage> swapchain_images;
        VkResult r = vkCreateViSurfaceNN(instance, &sci, nullptr, &surface);
        std::printf("[i] vkCreateViSurfaceNN -> %s\n", vk_result_str(r));

        if (r == VK_SUCCESS) {
            r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
            std::printf("[i] surface caps -> %s, extent=%ux%u, "
                        "minImageCount=%u, maxImageCount=%u\n",
                        vk_result_str(r),
                        caps.currentExtent.width, caps.currentExtent.height,
                        caps.minImageCount, caps.maxImageCount);
        }

        if (r == VK_SUCCESS) {
            uint32_t qf_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);

            std::vector<VkQueueFamilyProperties> qfs(qf_count);
            if (qf_count)
                vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count,
                                                             qfs.data());

            std::printf("[i] present support:\n");
            for (uint32_t i = 0; i < qf_count; i++) {
                VkBool32 supported = VK_FALSE;
                VkResult support_r =
                    vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface,
                                                            &supported);
                if (support_r == VK_SUCCESS && supported &&
                    present_qf_index == invalid_qf)
                    present_qf_index = i;
                std::printf("    qf[%u] flags=0x%x queues=%u -> %s (%s)\n",
                            i, qfs[i].queueFlags, qfs[i].queueCount,
                            supported ? "present" : "no-present",
                            vk_result_str(support_r));
            }
        }

        if (r == VK_SUCCESS) {
            uint32_t format_count = 0;
            VkResult format_r =
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface,
                                                        &format_count, nullptr);
            std::printf("[i] surface formats -> %s, count=%u\n",
                        vk_result_str(format_r), format_count);

            if (format_r == VK_SUCCESS && format_count) {
                surface_formats.resize(format_count);
                format_r = vkGetPhysicalDeviceSurfaceFormatsKHR(
                    pd, surface, &format_count, surface_formats.data());
                if (format_r == VK_SUCCESS) {
                    for (uint32_t i = 0; i < format_count; i++) {
                        std::printf("    format[%u] = VkFormat(%d), "
                                    "VkColorSpaceKHR(%d)\n",
                                    i, surface_formats[i].format,
                                    surface_formats[i].colorSpace);
                    }
                }
            }
        }

        if (r == VK_SUCCESS) {
            uint32_t mode_count = 0;
            VkResult mode_r =
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface,
                                                             &mode_count,
                                                             nullptr);
            std::printf("[i] present modes -> %s, count=%u\n",
                        vk_result_str(mode_r), mode_count);

            if (mode_r == VK_SUCCESS && mode_count) {
                present_modes.resize(mode_count);
                mode_r = vkGetPhysicalDeviceSurfacePresentModesKHR(
                    pd, surface, &mode_count, present_modes.data());
                if (mode_r == VK_SUCCESS) {
                    for (uint32_t i = 0; i < mode_count; i++) {
                        std::printf("    mode[%u] = %s (%d)\n",
                                    i, present_mode_str(present_modes[i]),
                                    present_modes[i]);
                    }
                }
            }
        }

        if (r == VK_SUCCESS) {
            uint32_t dev_ext_count = 0;
            VkResult dev_ext_r =
                vkEnumerateDeviceExtensionProperties(
                    pd, nullptr, &dev_ext_count, nullptr);
            std::printf("[i] device extensions -> %s, count=%u\n",
                        vk_result_str(dev_ext_r), dev_ext_count);

            if (dev_ext_r == VK_SUCCESS && dev_ext_count) {
                std::vector<VkExtensionProperties> dev_exts(dev_ext_count);
                dev_ext_r = vkEnumerateDeviceExtensionProperties(
                    pd, nullptr, &dev_ext_count, dev_exts.data());
                if (dev_ext_r == VK_SUCCESS) {
                    has_swapchain_ext =
                        has_extension(dev_exts,
                                      VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                    std::printf("    VK_KHR_swapchain: %s\n",
                                has_swapchain_ext ? "yes" : "no");
                }
            }
        }

        if (present_qf_index != invalid_qf) {
            const float priority = 1.0f;
            const char *device_exts[] = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            };
            VkDeviceQueueCreateInfo dqci = {};
            dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            dqci.queueFamilyIndex = present_qf_index;
            dqci.queueCount = 1;
            dqci.pQueuePriorities = &priority;

            VkDeviceCreateInfo dci = {};
            dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            dci.queueCreateInfoCount = 1;
            dci.pQueueCreateInfos = &dqci;
            if (has_swapchain_ext) {
                dci.enabledExtensionCount = 1;
                dci.ppEnabledExtensionNames = device_exts;
            }

            VkResult create_r =
                vkCreateDevice(pd, &dci, nullptr, &device);
            std::printf("[i] vkCreateDevice(qf=%u) -> %s\n",
                        present_qf_index, vk_result_str(create_r));

            if (create_r == VK_ERROR_INITIALIZATION_FAILED) {
                std::printf("    note: Switch NVK backend reached BO/VA "
                            "bring-up; the next likely missing piece is "
                            "GPU exec/signal submission\n");
            }

            if (create_r == VK_SUCCESS) {
                std::printf("    note: vkCreateDevice succeeded with the "
                            "current Switch bring-up stubs; real GPU "
                            "submission/bind paths are still incomplete\n");

                {
                    VkQueue queue = VK_NULL_HANDLE;
                    vkGetDeviceQueue(device, present_qf_index, 0, &queue);
                    std::printf("    queue[0]: %s\n",
                                queue ? "ok" : "null");

                    VkCommandPoolCreateInfo pool_ci = {};
                    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                    pool_ci.queueFamilyIndex = present_qf_index;

                    VkCommandPool command_pool = VK_NULL_HANDLE;
                    cmd_pool_r =
                        vkCreateCommandPool(device, &pool_ci, nullptr,
                                                &command_pool);
                    if (cmd_pool_r == VK_SUCCESS && command_pool) {
                        VkCommandBufferAllocateInfo alloc_ci = {};
                        alloc_ci.sType =
                            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                        alloc_ci.commandPool = command_pool;
                        alloc_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                        alloc_ci.commandBufferCount = 1;

                        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
                        cmd_alloc_r =
                            vkAllocateCommandBuffers(device, &alloc_ci,
                                                         &command_buffer);
                        if (cmd_alloc_r == VK_SUCCESS && command_buffer) {
                            VkCommandBufferBeginInfo begin_ci = {};
                            begin_ci.sType =
                                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

                            cmd_begin_r =
                                vkBeginCommandBuffer(command_buffer,
                                                         &begin_ci);
                            if (cmd_begin_r == VK_SUCCESS)
                                cmd_end_r =
                                    vkEndCommandBuffer(command_buffer);

                            if (cmd_end_r == VK_SUCCESS && queue) {
                                VkFenceCreateInfo fence_ci = {};
                                fence_ci.sType =
                                    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

                                VkFence submit_fence = VK_NULL_HANDLE;
                                VkResult fence_r =
                                    vkCreateFence(device, &fence_ci,
                                                      nullptr, &submit_fence);
                                if (fence_r == VK_SUCCESS && submit_fence) {
                                    VkSubmitInfo submit_info = {};
                                    submit_info.sType =
                                        VK_STRUCTURE_TYPE_SUBMIT_INFO;
                                    submit_info.commandBufferCount = 1;
                                    submit_info.pCommandBuffers = &command_buffer;

                                    submit_r =
                                        vkQueueSubmit(queue, 1,
                                                          &submit_info,
                                                          submit_fence);
                                    if (submit_r == VK_SUCCESS) {
                                        wait_submit_r =
                                            vkWaitForFences(device, 1,
                                                                 &submit_fence,
                                                                 VK_TRUE,
                                                                 UINT64_MAX);
                                    }

                                    vkDestroyFence(device, submit_fence,
                                                       nullptr);
                                } else {
                                    submit_r = fence_r;
                                }
                            }
                        }

                        vkDestroyCommandPool(device, command_pool, nullptr);
                    }
                }
            }
        }

        if (device && has_swapchain_ext &&
            !surface_formats.empty() && !present_modes.empty()) {
            uint32_t min_image_count = caps.minImageCount < 2 ? 2
                                                              : caps.minImageCount;
            if (caps.maxImageCount != 0 &&
                min_image_count > caps.maxImageCount) {
                min_image_count = caps.maxImageCount;
            }

            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX || extent.height == UINT32_MAX)
                extent = {1280, 720};

            VkImageUsageFlags usage = 0;
            if (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (usage == 0)
                usage = caps.supportedUsageFlags;

            VkSwapchainCreateInfoKHR swapchain_ci = {};
            swapchain_ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            swapchain_ci.surface = surface;
            swapchain_ci.minImageCount = min_image_count;
            swapchain_ci.imageFormat = surface_formats[0].format;
            swapchain_ci.imageColorSpace = surface_formats[0].colorSpace;
            swapchain_ci.imageExtent = extent;
            swapchain_ci.imageArrayLayers = 1;
            swapchain_ci.imageUsage = usage;
            swapchain_ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            swapchain_ci.preTransform = caps.currentTransform;
            swapchain_ci.compositeAlpha =
                choose_composite_alpha(caps.supportedCompositeAlpha);
            swapchain_ci.presentMode = choose_present_mode(present_modes);
            swapchain_ci.clipped = VK_TRUE;

            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            swapchain_probe_attempted = true;
            swapchain_extent = extent;
            swapchain_image_count = min_image_count;
            swapchain_mode = swapchain_ci.presentMode;
            if (run_visible_scanout_probe) {
                std::printf("[i] visual note: releasing console for Switch "
                            "WSI scanout probe\n");
                refresh_console();
                release_console();
            }

            swapchain_r =
                vkCreateSwapchainKHR(device, &swapchain_ci, nullptr,
                                         &swapchain);

            if (swapchain_r == VK_SUCCESS && swapchain) {
                swapchain_images_r =
                    vkGetSwapchainImagesKHR(device, swapchain,
                                            &swapchain_image_query_count,
                                                nullptr);

                if (swapchain_images_r == VK_SUCCESS &&
                    swapchain_image_query_count > 0) {
                    swapchain_images.resize(swapchain_image_query_count);
                    swapchain_image_list_count = swapchain_image_query_count;
                    swapchain_images_list_r =
                        vkGetSwapchainImagesKHR(device, swapchain,
                                                    &swapchain_image_list_count,
                                                    swapchain_images.data());
                    if (swapchain_images_list_r == VK_SUCCESS)
                        swapchain_images.resize(swapchain_image_list_count);
                }
            }

            if (!run_visible_scanout_probe &&
                swapchain_r == VK_SUCCESS && swapchain) {
                VkFenceCreateInfo fence_ci = {};
                fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

                VkFence fence = VK_NULL_HANDLE;
                VkResult fence_r =
                    vkCreateFence(device, &fence_ci, nullptr, &fence);
                if (fence_r == VK_SUCCESS && fence) {
                    acquire_r = vkAcquireNextImageKHR(device, swapchain,
                                                          UINT64_MAX,
                                                          VK_NULL_HANDLE,
                                                          fence,
                                                          &acquired_image_index);
                    if (acquire_r == VK_SUCCESS) {
                        wait_r = vkWaitForFences(device, 1, &fence,
                                                     VK_TRUE, UINT64_MAX);
                    }

                    vkDestroyFence(device, fence, nullptr);
                } else {
                    acquire_r = fence_r;
                }
            }

            if (run_present_only_probe &&
                swapchain_r == VK_SUCCESS && swapchain) {
                VkQueue queue = VK_NULL_HANDLE;
                vkGetDeviceQueue(device, present_qf_index, 0, &queue);

                VkSemaphoreCreateInfo semaphore_ci = {};
                semaphore_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
                VkResult semaphore_r =
                    vkCreateSemaphore(device, &semaphore_ci, nullptr,
                                          &acquire_semaphore);
                if (semaphore_r == VK_SUCCESS && acquire_semaphore && queue) {
                    acquire_present_r =
                        vkAcquireNextImageKHR(device, swapchain,
                                                  UINT64_MAX,
                                                  acquire_semaphore,
                                                  VK_NULL_HANDLE,
                                                  &presented_image_index);
                    if (acquire_present_r == VK_SUCCESS) {
                        VkSwapchainKHR present_swapchain = swapchain;
                        VkSemaphore present_wait = acquire_semaphore;
                        VkPresentInfoKHR present_info = {};
                        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                        present_info.waitSemaphoreCount = 1;
                        present_info.pWaitSemaphores = &present_wait;
                        present_info.swapchainCount = 1;
                        present_info.pSwapchains = &present_swapchain;
                        present_info.pImageIndices = &presented_image_index;
                        present_r = vkQueuePresentKHR(queue, &present_info);
                    }

                    vkDestroySemaphore(device, acquire_semaphore, nullptr);
                } else {
                    acquire_present_r = semaphore_r;
                }
            }

            if (swapchain_r == VK_SUCCESS && swapchain &&
                !swapchain_images.empty() &&
                (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
                if (!run_visible_scanout_probe) {
                    std::printf("[i] visual note: Switch WSI present is still "
                                "headless; keeping console active\n");
                    refresh_console();
                }

                VkQueue queue = VK_NULL_HANDLE;
                vkGetDeviceQueue(device, present_qf_index, 0, &queue);

                VkSemaphoreCreateInfo semaphore_ci = {};
                semaphore_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                VkFenceCreateInfo fence_ci = {};
                fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

                VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
                VkSemaphore render_semaphore = VK_NULL_HANDLE;
                VkFence render_fence = VK_NULL_HANDLE;
                VkCommandPool command_pool = VK_NULL_HANDLE;
                VkCommandBuffer command_buffer = VK_NULL_HANDLE;

                VkResult acquire_sem_r =
                    vkCreateSemaphore(device, &semaphore_ci, nullptr,
                                          &acquire_semaphore);
                VkResult render_sem_r =
                    vkCreateSemaphore(device, &semaphore_ci, nullptr,
                                          &render_semaphore);
                VkResult render_fence_r =
                    vkCreateFence(device, &fence_ci, nullptr, &render_fence);

                if (acquire_sem_r == VK_SUCCESS && render_sem_r == VK_SUCCESS &&
                    render_fence_r == VK_SUCCESS && acquire_semaphore &&
                    render_semaphore && render_fence && queue) {
                    acquire_clear_r =
                        vkAcquireNextImageKHR(device, swapchain,
                                                  UINT64_MAX,
                                                  acquire_semaphore,
                                                  VK_NULL_HANDLE,
                                                  &clear_image_index);

                    VkCommandPoolCreateInfo pool_ci = {};
                    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                    pool_ci.queueFamilyIndex = present_qf_index;
                    clear_pool_r =
                        vkCreateCommandPool(device, &pool_ci, nullptr,
                                                &command_pool);

                    if (acquire_clear_r == VK_SUCCESS &&
                        clear_image_index < swapchain_images.size() &&
                        clear_pool_r == VK_SUCCESS && command_pool) {
                        VkCommandBufferAllocateInfo alloc_ci = {};
                        alloc_ci.sType =
                            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                        alloc_ci.commandPool = command_pool;
                        alloc_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                        alloc_ci.commandBufferCount = 1;
                        clear_cmd_alloc_r =
                            vkAllocateCommandBuffers(device, &alloc_ci,
                                                         &command_buffer);

                        if (clear_cmd_alloc_r == VK_SUCCESS && command_buffer) {
                            VkCommandBufferBeginInfo begin_ci = {};
                            begin_ci.sType =
                                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                            clear_begin_r =
                                vkBeginCommandBuffer(command_buffer,
                                                         &begin_ci);

                            if (clear_begin_r == VK_SUCCESS) {
                                VkImageMemoryBarrier to_clear = {};
                                to_clear.sType =
                                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                                to_clear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                                to_clear.newLayout =
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                to_clear.srcQueueFamilyIndex =
                                    VK_QUEUE_FAMILY_IGNORED;
                                to_clear.dstQueueFamilyIndex =
                                    VK_QUEUE_FAMILY_IGNORED;
                                to_clear.image =
                                    swapchain_images[clear_image_index];
                                to_clear.subresourceRange.aspectMask =
                                    VK_IMAGE_ASPECT_COLOR_BIT;
                                to_clear.subresourceRange.baseMipLevel = 0;
                                to_clear.subresourceRange.levelCount = 1;
                                to_clear.subresourceRange.baseArrayLayer = 0;
                                to_clear.subresourceRange.layerCount = 1;
                                to_clear.dstAccessMask =
                                    VK_ACCESS_TRANSFER_WRITE_BIT;

                                vkCmdPipelineBarrier(
                                    command_buffer,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0,
                                    0, nullptr,
                                    0, nullptr,
                                    1, &to_clear);

                                VkClearColorValue clear_color = {{
                                    0.05f, 0.35f, 0.90f, 1.0f
                                }};
                                VkImageSubresourceRange clear_range = {};
                                clear_range.aspectMask =
                                    VK_IMAGE_ASPECT_COLOR_BIT;
                                clear_range.baseMipLevel = 0;
                                clear_range.levelCount = 1;
                                clear_range.baseArrayLayer = 0;
                                clear_range.layerCount = 1;

                                vkCmdClearColorImage(
                                    command_buffer,
                                    swapchain_images[clear_image_index],
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &clear_color,
                                    1, &clear_range);

                                VkImageMemoryBarrier to_present = {};
                                to_present.sType =
                                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                                to_present.oldLayout =
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                to_present.newLayout =
                                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                                to_present.srcQueueFamilyIndex =
                                    VK_QUEUE_FAMILY_IGNORED;
                                to_present.dstQueueFamilyIndex =
                                    VK_QUEUE_FAMILY_IGNORED;
                                to_present.image =
                                    swapchain_images[clear_image_index];
                                to_present.subresourceRange = clear_range;
                                to_present.srcAccessMask =
                                    VK_ACCESS_TRANSFER_WRITE_BIT;

                                vkCmdPipelineBarrier(
                                    command_buffer,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0,
                                    0, nullptr,
                                    0, nullptr,
                                    1, &to_present);

                                clear_cmd_end_r =
                                    vkEndCommandBuffer(command_buffer);
                            }

                            if (clear_cmd_end_r == VK_SUCCESS) {
                                VkPipelineStageFlags wait_stage =
                                    VK_PIPELINE_STAGE_TRANSFER_BIT;
                                VkSubmitInfo submit_info = {};
                                submit_info.sType =
                                    VK_STRUCTURE_TYPE_SUBMIT_INFO;
                                submit_info.waitSemaphoreCount = 1;
                                submit_info.pWaitSemaphores =
                                    &acquire_semaphore;
                                submit_info.pWaitDstStageMask = &wait_stage;
                                submit_info.commandBufferCount = 1;
                                submit_info.pCommandBuffers = &command_buffer;
                                submit_info.signalSemaphoreCount = 1;
                                submit_info.pSignalSemaphores =
                                    &render_semaphore;
                                clear_submit_r =
                                    vkQueueSubmit(queue, 1, &submit_info,
                                                      render_fence);

                                if (clear_submit_r == VK_SUCCESS) {
                                    VkSwapchainKHR present_swapchain = swapchain;
                                    VkSemaphore present_wait = render_semaphore;
                                    VkPresentInfoKHR present_info = {};
                                    present_info.sType =
                                        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                                    present_info.waitSemaphoreCount = 1;
                                    present_info.pWaitSemaphores =
                                        &present_wait;
                                    present_info.swapchainCount = 1;
                                    present_info.pSwapchains =
                                        &present_swapchain;
                                    present_info.pImageIndices =
                                        &clear_image_index;
                                    clear_present_r =
                                        vkQueuePresentKHR(queue,
                                                              &present_info);
                                    clear_wait_r =
                                        vkWaitForFences(device, 1,
                                                            &render_fence,
                                                            VK_TRUE,
                                                            UINT64_MAX);
                                    clear_queue_idle_r =
                                        vkQueueWaitIdle(queue);
                                    if (run_visible_scanout_probe &&
                                        clear_present_r == VK_SUCCESS &&
                                        clear_wait_r == VK_SUCCESS &&
                                        clear_queue_idle_r == VK_SUCCESS) {
                                        svcSleepThread(2000000000ULL);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    acquire_clear_r =
                        acquire_sem_r != VK_SUCCESS ? acquire_sem_r :
                        render_sem_r != VK_SUCCESS ? render_sem_r :
                        render_fence_r;
                }

                if (command_pool)
                    vkDestroyCommandPool(device, command_pool, nullptr);
                if (render_fence)
                    vkDestroyFence(device, render_fence, nullptr);
                if (render_semaphore)
                    vkDestroySemaphore(device, render_semaphore, nullptr);
                if (acquire_semaphore)
                    vkDestroySemaphore(device, acquire_semaphore, nullptr);
            }

            if (swapchain_r == VK_SUCCESS && swapchain)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
        }

        if (swapchain_probe_attempted) {
            init_console();
            flush_deferred_vk_messages();
            std::printf("[i] vkCreateSwapchainKHR -> %s, extent=%ux%u, "
                        "images=%u, mode=%s\n",
                        vk_result_str(swapchain_r),
                        swapchain_extent.width, swapchain_extent.height,
                        swapchain_image_count,
                        present_mode_str(swapchain_mode));
            if (swapchain_r == VK_SUCCESS) {
                std::printf("[i] vkGetSwapchainImagesKHR(count) -> %s, "
                            "count=%u\n",
                            vk_result_str(swapchain_images_r),
                            swapchain_image_query_count);
                if (swapchain_images_list_r == VK_SUCCESS) {
                    std::printf("[i] vkGetSwapchainImagesKHR(list) -> %s, "
                                "count=%u\n",
                                vk_result_str(swapchain_images_list_r),
                                swapchain_image_list_count);
                    for (uint32_t i = 0; i < swapchain_images.size(); i++) {
                        std::printf("    image[%u]: %s\n",
                                    i,
                                    swapchain_images[i] ? "ok" : "null");
                    }
                }
                if (!run_visible_scanout_probe) {
                    std::printf("[i] vkAcquireNextImageKHR -> %s",
                                vk_result_str(acquire_r));
                    if (acquire_r == VK_SUCCESS) {
                        std::printf(", index=%u\n", acquired_image_index);
                        std::printf("[i] vkWaitForFences -> %s\n",
                                    vk_result_str(wait_r));
                    } else {
                        std::printf("\n");
                    }
                }
                if (run_present_only_probe) {
                    std::printf("[i] vkAcquireNextImageKHR(present) -> %s",
                                vk_result_str(acquire_present_r));
                    if (acquire_present_r == VK_SUCCESS) {
                        std::printf(", index=%u\n", presented_image_index);
                        std::printf("[i] vkQueuePresentKHR -> %s\n",
                                    vk_result_str(present_r));
                    } else {
                        std::printf("\n");
                    }
                }
                std::printf("[i] vkCreateCommandPool -> %s\n",
                            vk_result_str(cmd_pool_r));
                std::printf("[i] vkAllocateCommandBuffers -> %s\n",
                            vk_result_str(cmd_alloc_r));
                std::printf("[i] vkBeginCommandBuffer -> %s\n",
                            vk_result_str(cmd_begin_r));
                std::printf("[i] vkEndCommandBuffer -> %s\n",
                            vk_result_str(cmd_end_r));
                std::printf("[i] vkQueueSubmit(empty) -> %s\n",
                            vk_result_str(submit_r));
                if (submit_r == VK_SUCCESS) {
                    std::printf("[i] vkWaitForFences(submit) -> %s\n",
                                vk_result_str(wait_submit_r));
                }
                std::printf("[i] vkAcquireNextImageKHR(clear) -> %s",
                            vk_result_str(acquire_clear_r));
                if (acquire_clear_r == VK_SUCCESS) {
                    std::printf(", index=%u\n", clear_image_index);
                } else {
                    std::printf("\n");
                }
                std::printf("[i] vkCreateCommandPool(clear) -> %s\n",
                            vk_result_str(clear_pool_r));
                std::printf("[i] vkAllocateCommandBuffers(clear) -> %s\n",
                            vk_result_str(clear_cmd_alloc_r));
                std::printf("[i] vkBeginCommandBuffer(clear) -> %s\n",
                            vk_result_str(clear_begin_r));
                std::printf("[i] vkEndCommandBuffer(clear) -> %s\n",
                            vk_result_str(clear_cmd_end_r));
                std::printf("[i] vkQueueSubmit(clear) -> %s\n",
                            vk_result_str(clear_submit_r));
                std::printf("[i] vkQueuePresentKHR(clear) -> %s\n",
                            vk_result_str(clear_present_r));
                if (clear_submit_r == VK_SUCCESS) {
                    std::printf("[i] vkWaitForFences(clear) -> %s\n",
                                vk_result_str(clear_wait_r));
                    std::printf("[i] vkQueueWaitIdle(clear) -> %s\n",
                                vk_result_str(clear_queue_idle_r));
                }
            }
            refresh_console();
        }

        if (device) {
            trace("pre vkDestroyDevice");
            vkDestroyDevice(device, nullptr);
            trace("post vkDestroyDevice");
        }

        if (surface) {
            trace("pre vkDestroySurfaceKHR");
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
    }

    refresh_console();
    destroy_instance(&instance_probe);

    trace("pre wait_for_exit");

    std::printf("[i] done.\n");

    wait_for_exit();
    trace("post wait_for_exit");
    release_console();
    return exit_code;
}
