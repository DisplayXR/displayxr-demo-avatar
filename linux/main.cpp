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
 * Windowing — APP-OWNED X11 WINDOW (transparent overlay, runtime#757). The app
 * creates a 32-bit ARGB X11 window and hands it to the runtime via
 * XR_DXR_xlib_window_binding with transparentBackgroundEnabled, so transparent
 * pixels compose through to the desktop — the transparent/click-through avatar
 * (the Lenovo use case). Falls back to hosted-NULL (graphics binding chained
 * straight in, runtime self-creates its window) when no X server / the extension
 * is absent (e.g. headless CI). Click-through is wired via an XShape input region
 * refreshed each frame from the avatar silhouette (clickthrough.cpp). The
 * macOS/Windows peers'
 * speech-bubble window-space layer (XR_DXR_local_3d_zone /
 * XrCompositionLayerWindowSpaceDXR) is likewise deferred to Phase 3 — this
 * build-green vehicle submits only the base projection layer.
 *
 * NOT a full port of the macOS/Windows app: no HUD, input, MCP tools, mode
 * switching, or auto-fit. Those live in the platform entry points and are added
 * when on-screen Linux support lands. This file exists to keep the demo
 * build-green on ubuntu-latest.
 */

#include <vulkan/vulkan.h>

// Xlib FIRST so XR_DXR_xlib_window_binding.h binds the real Display*/Window
// types (it provides stand-ins only when Xlib.h was not included). Needed for
// the app-owned-window path below.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// App-provided-window path: the avatar creates a 32-bit ARGB X11 window and
// hands it to the runtime via XR_DXR_xlib_window_binding with
// transparentBackgroundEnabled — the transparent/click-through overlay (the
// Lenovo use case). Falls back to hosted-NULL when no X server is available.
#include <openxr/XR_DXR_xlib_window_binding.h>
// Display-centric view rig: the runtime returns render-ready XrView{pose,fov}
// (camera at the eye, off-axis Kooima FOV) so the app doesn't hand-roll the view
// math. This is how modelviewer/gauss/cube-handle frame correctly on the
// native-VK path; the avatar consumes it the same way.
#include <openxr/XR_DXR_view_rig.h>

#include "clickthrough.h" // XShape silhouette click-through for the overlay

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <string>
#include <vector>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>

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

// out = a * b (column-major, OpenGL layout — same convention as above).
static void mat4_mul(float* out, const float* a, const float* b) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
}

// Model-fit transform: world = s * (model - center). The runtime's plain XR
// views are Kooima poses in METERS with the virtual display at the origin
// (~0.19 m tall panel, viewer at ~0.45 m) — a human-scale FBX drawn raw fills
// meters of world, which is why hardware validation saw only the lower legs
// (avatar#21). macOS/Windows fix framing by growing their virtual-display rig
// (ApplyAutoFitForLoadedScene); with plain views the equivalent is shrinking
// the model into panel scale.
static float g_fitMat[16];
static bool g_fitValid = false;

// Avatar target height in app units (≈ meters). The display view rig's virtual
// display is sized so the avatar fills ~80% of the panel — a framing choice, not
// a magic offset; the runtime rig + eye positions drive the actual view pose.
static constexpr float kAvatarFitHeightM = 0.15f;
static constexpr float kVirtualDisplayHeightM = kAvatarFitHeightM / 0.8f;

static void ComputeModelFit() {
    float center[3], extent[3];
    if (!g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent) || !(extent[1] > 1e-3f)) {
        return;
    }
    // Scale the human-scale FBX into panel units and center it at the display
    // plane (world origin). The view is owned by the display view rig now (camera
    // at the eye, off-axis FOV), so NO manual pose/offset is needed — the model at
    // the display plane renders in front of the rig's eye-distance camera.
    const float s = kAvatarFitHeightM / extent[1];
    for (int i = 0; i < 16; i++) g_fitMat[i] = 0.0f;
    g_fitMat[0] = g_fitMat[5] = g_fitMat[10] = s;
    g_fitMat[15] = 1.0f;
    g_fitMat[12] = -s * center[0];
    g_fitMat[13] = -s * center[1];
    g_fitMat[14] = -s * center[2];
    g_fitValid = true;
    LOG_INFO("Auto-fit: center=(%.3f,%.3f,%.3f) extentY=%.3f -> scale=%.5f (target %.2f m)",
             center[0], center[1], center[2], extent[1], s, kAvatarFitHeightM);
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

    // App-owned X11 window (transparent overlay). Null/0 → hosted-NULL fallback
    // (no X server, e.g. headless CI, or the xlib-binding extension is absent).
    Display* xDisplay = nullptr;
    Window xWindow = 0;
    Colormap xColormap = 0;
    unsigned int xWinW = 0, xWinH = 0; // app window size (for the click-through region)
    bool hasXlibBinding = false;       // XR_DXR_xlib_window_binding enabled on the instance
    bool usingAppWindow = false;       // the xlib binding was actually handed to xrCreateSession
    bool hasViewRig = false;           // XR_DXR_view_rig enabled — chain the display rig on locate
};

// Create a 32-bit ARGB X11 window for the transparent avatar overlay. Returns
// false (leaving xDisplay null) when no X server / no ARGB visual is available,
// so the caller falls back to hosted-NULL. A compositing WM (GNOME/Mutter, KWin,
// picom) must be running for the desktop to show through.
static bool CreateAppWindow(AppXrSession& xr) {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        LOG_INFO("XOpenDisplay failed (no X server) — using hosted-NULL windowing");
        return false;
    }
    int screen = DefaultScreen(dpy);

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        LOG_INFO("No 32-bit ARGB visual — using hosted-NULL windowing");
        XCloseDisplay(dpy);
        return false;
    }

    Window root = RootWindow(dpy, screen);
    Colormap cmap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    XSetWindowAttributes attrs = {};
    attrs.colormap = cmap;
    attrs.border_pixel = 0;      // required with a non-default colormap (else BadMatch)
    attrs.background_pixel = 0;  // fully-transparent fill
    attrs.event_mask = StructureNotifyMask | KeyPressMask;

    // Size to the screen for a full-panel overlay; on-hardware placement is
    // tuned separately (the app owns its window geometry on the xlib path).
    unsigned int w = (unsigned int)DisplayWidth(dpy, screen);
    unsigned int h = (unsigned int)DisplayHeight(dpy, screen);

    Window win = XCreateWindow(dpy, root, 0, 0, w, h, 0, 32, InputOutput, vinfo.visual,
                               CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
    if (win == 0) {
        LOG_ERROR("XCreateWindow failed — using hosted-NULL windowing");
        XFreeColormap(dpy, cmap);
        XCloseDisplay(dpy);
        return false;
    }
    XStoreName(dpy, win, "DisplayXR Avatar");
    XMapWindow(dpy, win);
    XFlush(dpy);

    xr.xDisplay = dpy;
    xr.xWindow = win;
    xr.xColormap = cmap;
    xr.xWinW = w;
    xr.xWinH = h;
    LOG_INFO("Created %ux%u 32-bit ARGB app window for transparent overlay", w, h);
    return true;
}

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    bool hasXlibBinding = false;
    bool hasViewRig = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0) hasXlibBinding = true;
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) hasViewRig = true;
    }

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }
    LOG_INFO("XR_DXR_xlib_window_binding: %s", hasXlibBinding ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_view_rig: %s", hasViewRig ? "AVAILABLE" : "NOT FOUND");

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    // Enable the app-owned-window binding when the runtime exposes it — required
    // to hand over our transparent X11 window (else we run hosted-NULL).
    if (hasXlibBinding) {
        enabledExtensions.push_back(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
        xr.hasXlibBinding = true;
    }
    // Enable the display view rig so the runtime returns render-ready view
    // poses/FOV (camera at the eye) instead of us hand-rolling the Kooima math.
    if (hasViewRig) {
        enabledExtensions.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
        xr.hasViewRig = true;
    }

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

    // App-owned-window path: chain the xlib binding (→ vkBinding) so the runtime
    // renders into our 32-bit ARGB window, with transparentBackgroundEnabled so
    // transparent pixels compose through to the desktop (the avatar overlay).
    // Falls back to hosted-NULL when no window / the extension is absent (e.g.
    // headless CI), where the graphics binding is chained straight in and the
    // runtime self-creates its window.
    XrXlibWindowBindingCreateInfoDXR xlibBinding = {XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR};
    xlibBinding.next = &vkBinding;
    xlibBinding.xDisplay = xr.xDisplay;
    xlibBinding.window = xr.xWindow;
    xlibBinding.transparentBackgroundEnabled = XR_TRUE;

    const bool useAppWindow = xr.hasXlibBinding && xr.xDisplay != nullptr && xr.xWindow != 0;
    xr.usingAppWindow = useAppWindow;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = useAppWindow ? (const void*)&xlibBinding : (const void*)&vkBinding;
    sessionInfo.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created (%s)",
             useAppWindow ? "app-owned ARGB window, transparent overlay"
                          : "hosted-NULL: runtime self-creates the window");
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
    // Tear down the app-owned X11 window after the runtime has released it.
    if (xr.xWindow != 0 && xr.xDisplay != nullptr) XDestroyWindow(xr.xDisplay, xr.xWindow);
    if (xr.xColormap != 0 && xr.xDisplay != nullptr) XFreeColormap(xr.xDisplay, xr.xColormap);
    if (xr.xDisplay != nullptr) XCloseDisplay(xr.xDisplay);
    xr.xWindow = 0;
    xr.xColormap = 0;
    xr.xDisplay = nullptr;
}

static void SignalHandler(int) { g_running = false; }

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    LOG_INFO("=== DisplayXR Avatar (Linux, app-owned ARGB window / transparent overlay) ===");

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

    // Best-effort: create the app-owned transparent ARGB window before the
    // session so CreateSession can hand it over. Falls back to hosted-NULL
    // (headless CI, no compositor) — never fatal.
    CreateAppWindow(xr);

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

    // Load a CLI-supplied model, else the bundled avatar (copied next to the
    // exe by CMake — resolve it against the EXE dir, not the CWD: the run
    // scripts exec from arbitrary directories, which is exactly the avatar#21
    // "no model auto-loads" failure), else the built-in debug model. Mirrors
    // modelviewer's ExeDir/TryAutoLoadBundledScene and the macOS
    // _NSGetExecutablePath / Windows GetModuleFileNameA peers.
    std::string modelPath = (argc > 1) ? argv[1] : "";
    if (modelPath.empty()) {
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string bundled = std::string(dirname(buf)) + "/avatar.fbx";
            struct stat st;
            if (stat(bundled.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                modelPath = bundled;
            }
        }
        if (modelPath.empty()) modelPath = "avatar.fbx"; // CWD fallback (old behavior)
    }
    if (g_modelRenderer.hasModel() || g_modelRenderer.loadModel(modelPath.c_str())) {
        LOG_INFO("Loaded model: %s", g_modelRenderer.modelPath().c_str());
    } else {
        LOG_WARN("No model at %s — using the built-in debug model", modelPath.c_str());
        g_modelRenderer.loadDebugModel();
    }
    g_modelRenderer.setPlainViewConvention(true);
    ComputeModelFit();

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

            // Chain the display view rig: the runtime returns render-ready
            // XrView{pose, fov} (camera AT the eye, off-axis Kooima FOV) so we
            // don't hand-roll the view math. This is the fix for the native-VK
            // path returning a display-plane (z≈0) view pose — with the rig the
            // pose sits at the eye (~0.5 m) like sim_display. Must be chained on
            // every locate (per-locate semantics). Identity pose = display plane
            // at the locate origin.
            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
            displayRig.pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
            displayRig.virtualDisplayHeight = kVirtualDisplayHeightM;
            displayRig.ipdFactor = 1.0f;
            displayRig.parallaxFactor = 1.0f;
            displayRig.perspectiveFactor = 1.0f;
            if (xr.hasViewRig) {
                locateInfo.next = &displayRig;
            }

            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 0;
            xrLocateViews(xr.session, &locateInfo, &viewState, 0, &viewCount, nullptr);
            if (viewCount == 0) viewCount = 2;
            std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
            XrResult loc = xrLocateViews(xr.session, &locateInfo, &viewState, viewCount, &viewCount, views.data());

            // Render whenever the locate succeeds — the view pose/FOV come from the
            // display rig above (render-ready), matching modelviewer/gaussiansplat.
            if (XR_SUCCEEDED(loc)) {
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
                        float silView[16], silProj[16];
                        bool haveSil = false;
                        for (uint32_t i = 0; i < eyeCount; i++) {
                            float viewMat[16], projMat[16];
                            mat4_view_from_xr_pose(viewMat, views[i].pose);
                            // Foreground clip (well-defined, no magic numbers): the far
                            // plane = the eye→display distance, so it coincides with the
                            // virtual display plane. The rig returns the eye at the
                            // rig-local z of the view pose (display plane = locate origin
                            // → |pose.z|). The avatar renders in front of the display; the
                            // desktop shows behind via compose-under-bg. Fall back to a far
                            // plane if the rig is absent / z is degenerate.
                            float ez = fabsf(views[i].pose.position.z);
                            float farZ = (ez > 0.02f) ? ez : 100.0f;
                            mat4_from_xr_fov(projMat, views[i].fov, 0.01f, farZ);
                            if (g_fitValid) { // bake the model-fit into the view (MV = V * F)
                                float fitted[16];
                                mat4_mul(fitted, viewMat, g_fitMat);
                                memcpy(viewMat, fitted, sizeof(fitted));
                            }
                            if (i == 0) { // view 0 drives the click-through silhouette
                                memcpy(silView, viewMat, sizeof(silView));
                                memcpy(silProj, projMat, sizeof(silProj));
                                haveSil = true;
                            }
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

                        // Refresh the click-through input region from the avatar
                        // silhouette (view 0). Only meaningful with an app-owned
                        // window; a no-op otherwise.
                        if (xr.usingAppWindow && haveSil) {
                            ClickthroughUpdate(vkDevice, physDevice, graphicsQueue, queueFamilyIndex,
                                               g_modelRenderer, xr.xDisplay, xr.xWindow, xr.xWinW, xr.xWinH,
                                               silView, silProj);
                        }
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
    // Teardown order matters (avatar#21 SIGABRT): release the global
    // renderer's device objects and idle the device BEFORE destroying it —
    // otherwise ~ModelRenderer's static-dtor cleanup() runs vkDeviceWaitIdle
    // + resource destroys on a dead VkDevice. Mirrors modelviewer.
    g_modelRenderer.cleanup();
    if (vkDevice) vkDeviceWaitIdle(vkDevice);
    if (vkDevice) ClickthroughDestroy(vkDevice); // silhouette scratch + pool
    CleanupOpenXR(xr);
    if (vkDevice) vkDestroyDevice(vkDevice, nullptr);
    if (vkInstance) vkDestroyInstance(vkInstance, nullptr);
    LOG_INFO("Clean exit");
    return 0;
}
