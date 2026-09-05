// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_DXR_win32_window_binding
 */

#include "xr_session.h"
#include "logging.h"
#include <openxr/XR_DXR_local_3d_zone.h>   // Local2D speech-bubble layer
#include <openxr/XR_DXR_view_rig.h>        // XR_DXR_VIEW_RIG_EXTENSION_NAME
#include <openxr/XR_DXR_depth_budget.h>    // XR_DXR_DEPTH_BUDGET_EXTENSION_NAME
#include <cstring>

// App-side availability flag (XrSessionManager carries no app-named fields).
// True when the runtime advertises XR_DXR_local_3d_zone, which the avatar uses
// to composite the 2D speech bubble as a mask-gated post-weave layer.
bool g_hasLocal3DZone = false;

// XR_DXR_view_rig + XR_DXR_display_zones: runtime-side framing for the tiger
// zone (the app chains XrDisplayZoneDXR + XrDisplayRigDXR on xrLocateViews and
// consumes render-ready XrView pose/fov instead of running app-side Kooima).
bool g_hasViewRigExt = false;
bool g_hasDisplayZonesExt = false;

// XR_DXR_depth_budget (#81): runtime-advisory rear depth budget for the
// transparent-standalone ZDP clip. Optional — older runtimes (including the
// one on this box today) don't advertise it, and the app must run unchanged.
bool g_hasDepthBudgetExt = false;
PFN_xrGetDisplayZoneCapabilitiesDXR g_pfnGetDisplayZoneCaps = nullptr;
PFN_xrGetDisplayZoneRecommendedViewSizeDXR g_pfnGetDisplayZoneViewSize = nullptr;

// XrDisplayDesktopPositionDXR (XR_DXR_display_info v16, runtime#715): 3D-panel
// top-left in virtual-desktop pixels. Zero-init = primary/unknown — an older
// runtime ignores the unknown chain entry and leaves (0,0) untouched, which is
// the safe pre-v16 behavior.
int32_t g_displayDesktopLeft = 0;
int32_t g_displayDesktopTop = 0;

// XR_DXR_mcp_tools (#30): app-owned extension flag + entry points (see
// xr_session.h). All NULL until InitializeOpenXR resolves them.
bool g_hasMcpToolsExt = false;
PFN_xrSetMCPAppInfoDXR      g_pfnSetMCPAppInfo = nullptr;
PFN_xrRegisterMCPToolDXR    g_pfnRegisterMCPTool = nullptr;
PFN_xrUnregisterMCPToolDXR  g_pfnUnregisterMCPTool = nullptr;

#define XR_CHECK(call) \
    do { \
        XrResult result = (call); \
        if (XR_FAILED(result)) { \
            LogXrResult(#call, result); \
            return false; \
        } \
    } while (0)

#define XR_CHECK_LOG(call) \
    do { \
        XrResult result = (call); \
        LogXrResult(#call, result); \
        if (XR_FAILED(result)) { \
            return false; \
        } \
    } while (0)

bool InitializeOpenXR(XrSessionManager& xr) {
    LOG_INFO("Querying OpenXR instance extension properties...");

    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    xr.hasWin32WindowBindingExt = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasWin32WindowBindingExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_WORKSPACE_FILE_DIALOG_EXTENSION_NAME) == 0) {
            xr.hasFileDialogExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0) {
            xr.hasAtlasCaptureExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME) == 0) {
            g_hasLocal3DZone = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) {
            g_hasViewRigExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_ZONES_EXTENSION_NAME) == 0) {
            g_hasDisplayZonesExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_MCP_TOOLS_EXTENSION_NAME) == 0) {
            g_hasMcpToolsExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_DEPTH_BUDGET_EXTENSION_NAME) == 0) {
            g_hasDepthBudgetExt = true;
        }
    }

    LOG_INFO("XR_KHR_vulkan_enable2: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_display_info: %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_workspace_file_dialog: %s", xr.hasFileDialogExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_atlas_capture: %s", xr.hasAtlasCaptureExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_local_3d_zone: %s", g_hasLocal3DZone ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_view_rig: %s", g_hasViewRigExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_display_zones: %s", g_hasDisplayZonesExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_mcp_tools: %s", g_hasMcpToolsExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_depth_budget: %s", g_hasDepthBudgetExt ? "AVAILABLE" : "NOT FOUND");

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable2 extension not available");
        return false;
    }

    // vulkan_enable2: the RUNTIME creates the VkInstance/VkDevice on our
    // behalf (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR), letting it
    // append the instance/device extensions AND enable the device features it
    // needs — notably VK_KHR_present_id/present_wait for late-weave pacing
    // (INV-5.9), with zero app-side feature code.
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (xr.hasWin32WindowBindingExt) {
        enabledExtensions.push_back(XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    }
    if (xr.hasFileDialogExt) {
        enabledExtensions.push_back(XR_DXR_WORKSPACE_FILE_DIALOG_EXTENSION_NAME);
    }
    if (xr.hasAtlasCaptureExt) {
        enabledExtensions.push_back(XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME);
    }
    if (g_hasLocal3DZone) {
        enabledExtensions.push_back(XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME);
    }
    if (g_hasViewRigExt) {
        enabledExtensions.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    }
    if (g_hasDisplayZonesExt) {
        enabledExtensions.push_back(XR_DXR_DISPLAY_ZONES_EXTENSION_NAME);
    }
    if (g_hasMcpToolsExt) {
        enabledExtensions.push_back(XR_DXR_MCP_TOOLS_EXTENSION_NAME);
    }
    if (g_hasDepthBudgetExt) {
        enabledExtensions.push_back(XR_DXR_DEPTH_BUDGET_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    // Shown as the client tile title in workspace mode — keep it the product
    // name, not the scaffold's internal codename.
    strcpy_s(createInfo.applicationInfo.applicationName, "DisplayXR 3D Avatar");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));
    LOG_INFO("System ID: %llu", (unsigned long long)xr.systemId);

    // Get system name
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
            LOG_INFO("System name: %s", xr.systemName);
        }
    }

    // Query display info via XR_DXR_display_info
    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
        XrEyeTrackingModeCapabilitiesDXR eyeCaps = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_DXR};
        // INV-1.3 (runtime#715): panel desktop position so the app window can
        // open ON the 3D panel. Zero-init: a pre-v16 runtime skips the unknown
        // chain entry and (0,0) = primary keeps the old placement.
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        displayInfo.next = &desktopPos;
        desktopPos.next = &eyeCaps;
        sysProps.next = &displayInfo;
        XrResult diResult = xrGetSystemProperties(xr.instance, xr.systemId, &sysProps);
        if (XR_SUCCEEDED(diResult)) {
            xr.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
            xr.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
            xr.displayWidthM = displayInfo.displaySizeMeters.width;
            xr.displayHeightM = displayInfo.displaySizeMeters.height;
            xr.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)eyeCaps.supportedModes;
            xr.defaultEyeTrackingMode = (uint32_t)eyeCaps.defaultMode;
            g_displayDesktopLeft = desktopPos.left;
            g_displayDesktopTop = desktopPos.top;
            LOG_INFO("Display desktop position: (%d, %d)", g_displayDesktopLeft, g_displayDesktopTop);
            LOG_INFO("Display info: scale=%.3fx%.3f, size=%.3fx%.3fm, pixels=%ux%u, nominal=(%.0f,%.0f,%.0f)mm",
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.nominalViewerX * 1000.0f, xr.nominalViewerY * 1000.0f, xr.nominalViewerZ * 1000.0f);
            LOG_INFO("Eye tracking: supported=0x%x, default=%u",
                xr.supportedEyeTrackingModes, xr.defaultEyeTrackingMode);
        }

        // Load xrRequestDisplayModeDXR function pointer
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);

        // Load xrRequestEyeTrackingModeDXR function pointer
        if (xr.supportedEyeTrackingModes != 0) {
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeDXR",
                (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);
        }

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    // #228 Tier 1 spatial file picker — resolve the app-side entrypoint
    // when the extension is enabled. Resolution failure is non-fatal: we
    // just fall through to the Win32 GetOpenFileNameA path at call time.
    if (xr.hasFileDialogExt) {
        xrGetInstanceProcAddr(xr.instance, "xrRequestFilePickerDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestFilePickerEXT);
        LOG_INFO("xrRequestFilePickerDXR: %s",
            xr.pfnRequestFilePickerEXT ? "resolved" : "NULL");
    }

    // XR_DXR_atlas_capture (W6 of #396): resolve the runtime-owned capture entry.
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasDXR",
            (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
        LOG_INFO("xrCaptureAtlasDXR: %s", xr.pfnCaptureAtlasEXT ? "resolved" : "NULL");
    }

    // XR_DXR_display_zones (ADR-027): resolve the zone caps + per-rect
    // recommended-view-size entries. Both are session-scoped calls used by
    // the zones frame path in main.cpp.
    if (g_hasDisplayZonesExt) {
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneCapabilitiesDXR",
            (PFN_xrVoidFunction*)&g_pfnGetDisplayZoneCaps);
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneRecommendedViewSizeDXR",
            (PFN_xrVoidFunction*)&g_pfnGetDisplayZoneViewSize);
        LOG_INFO("xrGetDisplayZoneCapabilitiesDXR: %s / xrGetDisplayZoneRecommendedViewSizeDXR: %s",
            g_pfnGetDisplayZoneCaps ? "resolved" : "NULL",
            g_pfnGetDisplayZoneViewSize ? "resolved" : "NULL");
    }

    // XR_DXR_mcp_tools (#30): resolve the agent-tool entry points. The appId +
    // base tools are declared after xrCreateSession (RegisterMcpBaseTools in
    // main.cpp), the animation tools are late-(un)registered on model load, and
    // tool calls are dispatched by the shared displayxr-common PollEvents via
    // the app-supplied mcpToolHandler hook (common #18). Resolution failure is
    // non-fatal by design — the MCP capability gate may simply be off; the
    // avatar runs identically without an agent surface.
    //
    // The registration entry points are app-owned globals; the arg-fetch +
    // result-submit ones go into the session manager (xr.pfn*EXT), which is
    // where the shared PollEvents reads them — the app never calls them itself.
    if (g_hasMcpToolsExt) {
        xrGetInstanceProcAddr(xr.instance, "xrSetMCPAppInfoDXR",
            (PFN_xrVoidFunction*)&g_pfnSetMCPAppInfo);
        xrGetInstanceProcAddr(xr.instance, "xrRegisterMCPToolDXR",
            (PFN_xrVoidFunction*)&g_pfnRegisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrUnregisterMCPToolDXR",
            (PFN_xrVoidFunction*)&g_pfnUnregisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrGetMCPToolCallArgsDXR",
            (PFN_xrVoidFunction*)&xr.pfnGetMCPToolCallArgsEXT);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitMCPToolResultDXR",
            (PFN_xrVoidFunction*)&xr.pfnSubmitMCPToolResultEXT);
        const bool resolved = g_pfnSetMCPAppInfo && g_pfnRegisterMCPTool &&
            g_pfnUnregisterMCPTool && xr.pfnGetMCPToolCallArgsEXT &&
            xr.pfnSubmitMCPToolResultEXT;
        xr.hasMcpToolsExt = resolved;
        LOG_INFO("XR_DXR_mcp_tools entry points: %s", resolved ? "resolved" : "NULL");
        if (!resolved) g_hasMcpToolsExt = false;  // defensive: treat partial resolve as absent
    } else {
        LOG_INFO("XR_DXR_mcp_tools: not advertised by runtime");
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

bool GetVulkanGraphicsRequirements(XrSessionManager& xr) {
    LOG_INFO("Getting Vulkan graphics requirements...");

    // vulkan_enable2: the v2 proc name — the v1 name is only dispatchable when
    // XR_KHR_vulkan_enable (v1) is the enabled extension.
    PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanGraphicsRequirements2KHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirements2KHR",
        (PFN_xrVoidFunction*)&xrGetVulkanGraphicsRequirements2KHR);
    if (XR_FAILED(result) || !xrGetVulkanGraphicsRequirements2KHR) {
        LOG_ERROR("Failed to get xrGetVulkanGraphicsRequirements2KHR function pointer");
        return false;
    }

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    result = xrGetVulkanGraphicsRequirements2KHR(xr.instance, xr.systemId, &graphicsReq);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetVulkanGraphicsRequirements2KHR", result);
        return false;
    }

    LOG_INFO("Vulkan graphics requirements:");
    LOG_INFO("  Min API version: %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.minApiVersionSupported));
    LOG_INFO("  Max API version: %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

    return true;
}

bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance) {
    LOG_INFO("Creating Vulkan instance via xrCreateVulkanInstanceKHR (vulkan_enable2)...");

    PFN_xrCreateVulkanInstanceKHR xrCreateVulkanInstanceKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrCreateVulkanInstanceKHR",
        (PFN_xrVoidFunction*)&xrCreateVulkanInstanceKHR);
    if (XR_FAILED(result) || !xrCreateVulkanInstanceKHR) {
        LOG_ERROR("Failed to get xrCreateVulkanInstanceKHR");
        return false;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SR3DGSOpenXRExtVK";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;  // 3DGS.cpp needs Vulkan 1.2+

    // The runtime appends whatever instance extensions it needs.
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    XrVulkanInstanceCreateInfoKHR xrCreateInfo = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrCreateInfo.systemId = xr.systemId;
    xrCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrCreateInfo.vulkanCreateInfo = &createInfo;

    VkResult vkResult = VK_SUCCESS;
    result = xrCreateVulkanInstanceKHR(xr.instance, &xrCreateInfo, &vkInstance, &vkResult);
    if (XR_FAILED(result) || vkResult != VK_SUCCESS) {
        LOG_ERROR("xrCreateVulkanInstanceKHR failed: xr=%d vk=%d", result, vkResult);
        return false;
    }

    LOG_INFO("Vulkan instance created (runtime-managed extensions)");
    return true;
}

bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    LOG_INFO("Getting Vulkan physical device via xrGetVulkanGraphicsDevice2KHR...");

    PFN_xrGetVulkanGraphicsDevice2KHR xrGetVulkanGraphicsDevice2KHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDevice2KHR",
        (PFN_xrVoidFunction*)&xrGetVulkanGraphicsDevice2KHR);
    if (XR_FAILED(result) || !xrGetVulkanGraphicsDevice2KHR) {
        LOG_ERROR("Failed to get xrGetVulkanGraphicsDevice2KHR");
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR getInfo = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    getInfo.systemId = xr.systemId;
    getInfo.vulkanInstance = vkInstance;

    result = xrGetVulkanGraphicsDevice2KHR(xr.instance, &getInfo, &physDevice);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetVulkanGraphicsDevice2KHR", result);
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    LOG_INFO("  API version: %d.%d.%d",
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    return true;
}

bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            LOG_INFO("Graphics queue family: %u", i);
            return true;
        }
    }

    LOG_ERROR("No graphics queue family found");
    return false;
}

bool CreateVulkanDevice(XrSessionManager& xr, VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    VkDevice& device, VkQueue& graphicsQueue)
{
    LOG_INFO("Creating Vulkan device via xrCreateVulkanDeviceKHR (vulkan_enable2)...");

    PFN_xrCreateVulkanDeviceKHR xrCreateVulkanDeviceKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrCreateVulkanDeviceKHR",
        (PFN_xrVoidFunction*)&xrCreateVulkanDeviceKHR);
    if (XR_FAILED(result) || !xrCreateVulkanDeviceKHR) {
        LOG_ERROR("Failed to get xrCreateVulkanDeviceKHR");
        return false;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // Query supported features before requesting them. NOTE: narrow
    // VkPhysicalDeviceShaderAtomicInt64Features, NOT the aggregate
    // VkPhysicalDeviceVulkan12Features — the runtime chains its own
    // VkPhysicalDeviceTimelineSemaphoreFeatures onto our create-info, and an
    // aggregate 1.2-features struct cannot legally coexist with per-feature
    // structs in one pNext chain (VUID-VkDeviceCreateInfo-pNext-02830).
    VkPhysicalDeviceShaderAtomicInt64Features supportedAtomic64 = {};
    supportedAtomic64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
    VkPhysicalDeviceFeatures2 supportedFeatures2 = {};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures2.pNext = &supportedAtomic64;
    vkGetPhysicalDeviceFeatures2(physDevice, &supportedFeatures2);

    // Enable features required by 3DGS compute shaders (only if available)
    VkPhysicalDeviceFeatures features = {};
    features.shaderInt64 = supportedFeatures2.features.shaderInt64;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    // 64-bit atomics are optional — sort.comp uses CAS fallback when unavailable
    VkPhysicalDeviceShaderAtomicInt64Features atomic64Features = {};
    atomic64Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
    atomic64Features.shaderBufferInt64Atomics = supportedAtomic64.shaderBufferInt64Atomics;
    atomic64Features.shaderSharedInt64Atomics = supportedAtomic64.shaderSharedInt64Atomics;

    // The runtime appends its required device extensions AND enables the
    // features it needs — timelineSemaphore (the shell IPC path's
    // cross-process workspace_sync_fence import) and present_id/present_wait
    // for late-weave pacing (INV-5.9). Only the app's own 3DGS features
    // remain here; the app-side timelineSemaphore enable is gone.
    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &atomic64Features;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.pEnabledFeatures = &features;

    XrVulkanDeviceCreateInfoKHR xrCreateInfo = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrCreateInfo.systemId = xr.systemId;
    xrCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrCreateInfo.vulkanPhysicalDevice = physDevice;
    xrCreateInfo.vulkanCreateInfo = &createInfo;

    VkResult vkResult = VK_SUCCESS;
    result = xrCreateVulkanDeviceKHR(xr.instance, &xrCreateInfo, &device, &vkResult);
    if (XR_FAILED(result) || vkResult != VK_SUCCESS) {
        LOG_ERROR("xrCreateVulkanDeviceKHR failed: xr=%d vk=%d", result, vkResult);
        return false;
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    LOG_INFO("Vulkan device and graphics queue created");
    return true;
}

bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd)
{
    LOG_INFO("Creating OpenXR session with Vulkan + XR_DXR_win32_window_binding...");

    xr.windowHandle = hwnd;

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = queueIndex;

    XrWin32WindowBindingCreateInfoDXR sessionTarget = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR};
    sessionTarget.windowHandle = hwnd;
    // Always-on transparent-window support. The runtime wires DComp + the
    // KMT-shared-texture bridge based on these fields at xrCreateSession;
    // they cannot be flipped at runtime. Ctrl+T at the app level only
    // changes the renderer's output alpha — the chroma-key strip pass is
    // a no-op when alpha == 1 throughout, so opaque mode looks identical
    // to a non-transparent session. Requires runtime ≥ v1.3.0.
    sessionTarget.transparentBackgroundEnabled = XR_TRUE;

    if (xr.hasWin32WindowBindingExt && hwnd) {
        vkBinding.next = &sessionTarget;
        LOG_INFO("Using XR_DXR_win32_window_binding with window handle (transparent-bg ENABLED)");
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeIsRequestable[i] = modes[i].isRequestable ? true : false;
                    // v13 initial-mode-sync: trust runtime-reported active mode.
                    if (modes[i].isActive) {
                        xr.currentModeIndex = modes[i].modeIndex;
                    }
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, tiles=%ux%u, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY,
                        xr.renderingModeTileColumns[i], xr.renderingModeTileRows[i],
                        modes[i].hardwareDisplay3D);
                }
            }
        }
    }

    return true;
}
