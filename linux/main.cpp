// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux entry point for the DisplayXR avatar demo (build-green, #21).
 *
 * The avatar demo has per-platform entry points — macos/main.mm (Cocoa) and
 * windows/main.cpp (Win32). This is the Linux sibling. It drives the SAME
 * vendor-neutral, cross-platform ModelRenderer (model_common/) through a
 * minimal Vulkan + OpenXR frame loop, so a Linux CI job compiles and links the
 * heavy renderer stack (tinygltf / tinyusdz / ufbx / glm / SPIR-V shaders) that
 * is the real portability surface of this demo.
 *
 * Windowing — HOSTED-NULL (Phase-1 interim). Per docs/guides/linux-demo-port.md
 * this app passes NO window binding at xrCreateSession: the graphics binding is
 * chained straight into XrSessionCreateInfo and the runtime self-creates its
 * window. The faithful _handle path (app-owned X11 window handed over via
 * XR_EXT_xlib_window_binding — the vendored header is already in
 * openxr_includes/) is Phase-3 work, gated on the Linux runtime + a GPU + an X
 * server; see the TODO(Phase 3) block in main(). The macOS/Windows peers'
 * speech-bubble window-space layer (XR_EXT_local_3d_zone /
 * XrCompositionLayerWindowSpaceEXT) is likewise deferred to Phase 3 — this
 * build-green vehicle submits only the base projection layer.
 *
 * NOT a full port of the macOS/Windows app: no HUD, input, MCP tools, mode
 * switching, or auto-fit. Those live in the platform entry points and are added
 * when on-screen Linux support lands. This file exists to keep the demo
 * build-green on ubuntu-latest.
 */

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// Vendored, forward-looking: the real app-provided-window path for Phase 3.
// Unused in the hosted-NULL build below (no window binding is chained).
#include <openxr/XR_EXT_xlib_window_binding.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <csignal>
#include <chrono>
#include <string>
#include <vector>
#include <unistd.h>

#include "model_renderer.h"
#include "model_loader.h"

// ============================================================================
// Logging
// ============================================================================
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call)                                                     \
    do {                                                                   \
        XrResult _r = (call);                                              \
        if (XR_FAILED(_r)) {                                               \
            LOG_ERROR("%s failed: %d", #call, (int)_r);                    \
            return false;                                                  \
        }                                                                  \
    } while (0)

#define VK_CHECK(call)                                                     \
    do {                                                                   \
        VkResult _r = (call);                                              \
        if (_r != VK_SUCCESS) {                                            \
            LOG_ERROR("%s failed: %d", #call, (int)_r);                    \
            return false;                                                  \
        }                                                                  \
    } while (0)

static volatile bool g_running = true;
static ModelRenderer g_modelRenderer;

// ============================================================================
// Matrix helpers (column-major, m[col*4 + row]) — verbatim shape from the
// runtime's cube_handle_vk_linux; only the view/projection builders are needed.
// ============================================================================
static void mat4_from_xr_fov(float* m, const XrFovf& fov, float nearZ, float farZ) {
    float left = nearZ * tanf(fov.angleLeft);
    float right = nearZ * tanf(fov.angleRight);
    float top = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);

    float w = right - left;
    float h = top - bottom;

    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f * nearZ / w;
    m[5]  = 2.0f * nearZ / h;
    m[8]  = (right + left) / w;
    m[9]  = (top + bottom) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* m, const XrPosef& pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
    float r01 = 2.0f * (qx * qy - qz * qw);
    float r02 = 2.0f * (qx * qz + qy * qw);
    float r10 = 2.0f * (qx * qy + qz * qw);
    float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
    float r12 = 2.0f * (qy * qz - qx * qw);
    float r20 = 2.0f * (qx * qz - qy * qw);
    float r21 = 2.0f * (qy * qz + qx * qw);
    float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    float tx = -(r00 * px + r10 * py + r20 * pz);
    float ty = -(r01 * px + r11 * py + r21 * pz);
    float tz = -(r02 * px + r12 * py + r22 * pz);

    m[0]  = r00;  m[4]  = r10;  m[8]  = r20;  m[12] = tx;
    m[1]  = r01;  m[5]  = r11;  m[9]  = r21;  m[13] = ty;
    m[2]  = r02;  m[6]  = r12;  m[10] = r22;  m[14] = tz;
    m[3]  = 0.0f; m[7]  = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

// ============================================================================
// OpenXR session state
// ============================================================================
struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    SwapchainInfo swapchain;

    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    uint32_t viewWidth = 0;
    uint32_t viewHeight = 0;
};

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    for (const auto& ext : extensions)
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "AvatarHandleVkLinux",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "None",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();
    XR_CHECK(xrCreateInstance(&createInfo, &xr.instance));

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));
    xr.viewWidth = xr.configViews[0].recommendedImageRectWidth;
    xr.viewHeight = xr.configViews[0].recommendedImageRectHeight;
    LOG_INFO("View config: %u views, per-view %ux%u", viewCount, xr.viewWidth, xr.viewHeight);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfn));
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(xr.instance, xr.systemId, &req));
    return true;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&pfn));
    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extStr.data());

    std::vector<std::string> names;
    { size_t start = 0;
      while (start < extStr.size()) {
          size_t end = extStr.find(' ', start);
          if (end == std::string::npos) end = extStr.size();
          std::string n = extStr.substr(start, end - start);
          if (!n.empty() && n[0] != '\0') names.push_back(n);
          start = end + 1;
      } }

    std::vector<const char*> ptrs;
    for (auto& n : names) ptrs.push_back(n.c_str());

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "AvatarHandleVkLinux";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)ptrs.size();
    createInfo.ppEnabledExtensionNames = ptrs.data();
    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    PFN_xrGetVulkanGraphicsDeviceKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&pfn));
    XR_CHECK(pfn(xr.instance, xr.systemId, vkInstance, &physDevice));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& deviceExtensions,
                                      std::vector<std::string>& storage) {
    PFN_xrGetVulkanDeviceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&pfn));
    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extStr.data());

    storage.clear();
    deviceExtensions.clear();
    { size_t start = 0;
      while (start < extStr.size()) {
          size_t end = extStr.find(' ', start);
          if (end == std::string::npos) end = extStr.size();
          std::string n = extStr.substr(start, end - start);
          if (!n.empty() && n[0] != '\0') storage.push_back(n);
          start = end + 1;
      } }
    for (auto& n : storage) deviceExtensions.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, families.data());
    for (uint32_t i = 0; i < count; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueFamilyIndex = i; return true; }
    LOG_ERROR("No graphics queue family found");
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
                               const std::vector<const char*>& deviceExtensions,
                               VkDevice& device, VkQueue& graphicsQueue) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();
    VK_CHECK(vkCreateDevice(physDevice, &createInfo, nullptr, &device));
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
                          VkDevice device, uint32_t queueFamilyIndex) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = 0;

    // HOSTED-NULL: the graphics binding is chained straight into the session
    // create info — NO window binding. The runtime self-creates its window.
    //
    // TODO(Phase 3): become a faithful _handle app. Create an app-owned X11
    // window (XOpenDisplay / XCreateWindow), then chain
    //   XrXlibWindowBindingCreateInfoEXT { .next = &vkBinding, .xDisplay, .window }
    // (header already vendored: openxr/XR_EXT_xlib_window_binding.h) as
    // sessionInfo.next, and enable XR_EXT_XLIB_WINDOW_BINDING_EXTENSION_NAME in
    // InitializeOpenXR. Gated on the Linux runtime + a GPU + an X server —
    // docs/guides/linux-demo-port.md, runtime #660/#699.
    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created (hosted-NULL: runtime self-creates the window)");
    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo localInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &localInfo, &xr.localSpace));

    XrReferenceSpaceCreateInfo viewInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &viewInfo, &xr.viewSpace));
    return true;
}

static bool CreateSwapchain(AppXrSession& xr) {
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));
    int64_t selectedFormat = formats.empty() ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : formats[0];

    const auto& view = xr.configViews[0];
    uint32_t scWidth = view.recommendedImageRectWidth * 2; // stereo SBS atlas
    uint32_t scHeight = view.recommendedImageRectHeight;

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = scWidth;
    swapchainInfo.height = scHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchain.swapchain));

    xr.swapchain.format = selectedFormat;
    xr.swapchain.width = scWidth;
    xr.swapchain.height = scHeight;
    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imageCount, nullptr));
    xr.swapchain.imageCount = imageCount;
    LOG_INFO("Atlas swapchain: %ux%u, %u images", scWidth, scHeight, imageCount);
    return true;
}

static bool PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = e->state;
            switch (xr.sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = xr.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(xr.session, &beginInfo))) {
                    xr.sessionRunning = true;
                    LOG_INFO("Session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                xr.exitRequested = true;
                break;
            default: break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            xr.exitRequested = true;
            break;
        default: break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.viewSpace != XR_NULL_HANDLE) xrDestroySpace(xr.viewSpace);
    if (xr.localSpace != XR_NULL_HANDLE) xrDestroySpace(xr.localSpace);
    if (xr.session != XR_NULL_HANDLE) xrDestroySession(xr.session);
    if (xr.instance != XR_NULL_HANDLE) xrDestroyInstance(xr.instance);
}

static void SignalHandler(int) { g_running = false; }

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    LOG_INFO("=== DisplayXR Avatar (Linux, hosted-NULL, build-green) ===");

    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }
    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }
    // Negative-viewport Y-flip (matches the macOS/Windows raster convention).
    devExts.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    if (!CreateSpaces(xr) || !CreateSwapchain(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    std::vector<XrSwapchainImageVulkanKHR> swapchainImages(
        xr.swapchain.imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    { uint32_t count = xr.swapchain.imageCount;
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    // The cross-platform PBR renderer — the real portability surface exercised
    // by this build-green vehicle. Per-eye render target dimensions.
    if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                              queueFamilyIndex, xr.viewWidth, xr.viewHeight))
        LOG_WARN("model renderer init failed (no GPU on CI — expected)");

    // Load the bundled avatar if present (copied next to the exe by CMake), or
    // a CLI-supplied model, else the built-in debug model.
    const char* modelPath = (argc > 1) ? argv[1] : "avatar.fbx";
    if (g_modelRenderer.hasModel() || g_modelRenderer.loadModel(modelPath)) {
        LOG_INFO("Loaded model: %s", g_modelRenderer.modelPath().c_str());
    } else {
        g_modelRenderer.loadDebugModel();
    }
    g_modelRenderer.setPlainViewConvention(true);

    LOG_INFO("=== Entering main loop (Ctrl+C to exit) ===");
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PollEvents(xr);

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        g_modelRenderer.updateAnimation(dt);

        if (!xr.sessionRunning) { usleep(100000); continue; }

        XrFrameState frameState = {XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(xr.session, &waitInfo, &frameState))) { xr.exitRequested = true; break; }
        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(xr.session, &beginInfo);

        bool rendered = false;
        std::vector<XrCompositionLayerProjectionView> projViews;

        if (frameState.shouldRender) {
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 0;
            xrLocateViews(xr.session, &locateInfo, &viewState, 0, &viewCount, nullptr);
            if (viewCount == 0) viewCount = 2;
            std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
            XrResult loc = xrLocateViews(xr.session, &locateInfo, &viewState, viewCount, &viewCount, views.data());

            if (XR_SUCCEEDED(loc) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                uint32_t imageIndex = 0;
                if (XR_SUCCEEDED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &acqInfo, &imageIndex))) {
                    XrSwapchainImageWaitInfo swWait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    swWait.timeout = XR_INFINITE_DURATION;
                    if (XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &swWait))) {
                        rendered = true;
                        uint32_t eyeCount = 2;
                        uint32_t eyeW = xr.viewWidth, eyeH = xr.viewHeight;
                        projViews.resize(eyeCount, {});
                        for (uint32_t i = 0; i < eyeCount; i++) {
                            float viewMat[16], projMat[16];
                            mat4_view_from_xr_pose(viewMat, views[i].pose);
                            mat4_from_xr_fov(projMat, views[i].fov, 0.01f, 100.0f);
                            // Draw the avatar into this eye's SBS viewport region.
                            g_modelRenderer.renderEye(
                                swapchainImages[imageIndex].image,
                                (VkFormat)xr.swapchain.format,
                                xr.swapchain.width, xr.swapchain.height,
                                i * eyeW, 0, eyeW, eyeH,
                                viewMat, projMat,
                                /*transparentBg*/ true);

                            projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projViews[i].subImage.swapchain = xr.swapchain.swapchain;
                            projViews[i].subImage.imageRect.offset = {(int32_t)(i * eyeW), 0};
                            projViews[i].subImage.imageRect.extent = {(int32_t)eyeW, (int32_t)eyeH};
                            projViews[i].subImage.imageArrayIndex = 0;
                            projViews[i].pose = views[i].pose;
                            projViews[i].fov = views[i].fov;
                        }
                        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        xrReleaseSwapchainImage(xr.swapchain.swapchain, &relInfo);
                    }
                }
            }
        }

        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1];
        if (rendered) {
            projLayer.space = xr.localSpace;
            projLayer.viewCount = (uint32_t)projViews.size();
            projLayer.views = projViews.data();
            layers[0] = (XrCompositionLayerBaseHeader*)&projLayer;
            endInfo.layerCount = 1;
            endInfo.layers = layers;
        }
        xrEndFrame(xr.session, &endInfo);
    }

    LOG_INFO("=== Shutting down ===");
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    return 0;
}
