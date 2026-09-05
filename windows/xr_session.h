// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_DXR_win32_window_binding
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr_session_common.h"
#include <openxr/XR_DXR_display_zones.h>  // zone caps/view-size PFN typedefs

// True when the runtime advertises XR_DXR_local_3d_zone (set in InitializeOpenXR).
// Gates the avatar's 2D speech-bubble Local2D layer.
extern bool g_hasLocal3DZone;

// True when the runtime advertises XR_DXR_view_rig (set in InitializeOpenXR).
// Required for the zone-chained XrDisplayRigDXR locate (runtime-side framing).
extern bool g_hasViewRigExt;

// True when the runtime advertises XR_DXR_display_zones (set in InitializeOpenXR).
// Together with g_hasViewRigExt this gates the zones frame path: one 3D zone
// (the tiger, bottom 75%) framed by the runtime rig instead of app-side Kooima.
extern bool g_hasDisplayZonesExt;

// True when the runtime advertises XR_DXR_depth_budget (set in InitializeOpenXR).
// An older runtime lacks the extension entirely — the app must keep running
// with today's hand-rolled ZDP clip unchanged (dxr::ResolveClipPlanes' no-budget
// fallback), never fail to start. See displayxr-demo-avatar#81.
extern bool g_hasDepthBudgetExt;

// XR_DXR_display_zones entry points (resolved in InitializeOpenXR; NULL when
// the extension is absent — callers must check).
extern PFN_xrGetDisplayZoneCapabilitiesDXR g_pfnGetDisplayZoneCaps;
extern PFN_xrGetDisplayZoneRecommendedViewSizeDXR g_pfnGetDisplayZoneViewSize;

// XR_DXR_mcp_tools (#30): app-defined agent tools on the runtime-hosted
// per-process MCP server. Set/resolved in InitializeOpenXR; registration
// happens after xrCreateSession (main.cpp) and tool-call dispatch runs through
// the shared displayxr-common PollEvents via the app-supplied mcpToolHandler
// hook (displayxr-common v2.1.0 / common #18). The whole path is inert (all
// NULL) when the runtime doesn't advertise the extension or the MCP capability
// gate is off — never load-bearing.
//
// These three registration entry points stay app-owned globals (the shared
// XrSessionManager has no xrUnregisterMCPToolDXR field). The arg-fetch +
// result-submit entry points that PollEvents itself uses are resolved into the
// session manager (xr.pfnGetMCPToolCallArgsEXT / xr.pfnSubmitMCPToolResultEXT),
// NOT into app globals — the handler must never touch them (#18).
extern bool g_hasMcpToolsExt;
extern PFN_xrSetMCPAppInfoDXR      g_pfnSetMCPAppInfo;
extern PFN_xrRegisterMCPToolDXR    g_pfnRegisterMCPTool;
extern PFN_xrUnregisterMCPToolDXR  g_pfnUnregisterMCPTool;

// 3D-panel top-left in virtual-desktop pixels (XrDisplayDesktopPositionDXR,
// XR_DXR_display_info v16, runtime#715), captured in InitializeOpenXR. (0,0)
// = primary monitor or unknown (older runtime) — the safe default.
extern int32_t g_displayDesktopLeft;
extern int32_t g_displayDesktopTop;

// Initialize OpenXR instance with Vulkan (vulkan_enable2) + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get Vulkan graphics requirements (xrGetVulkanGraphicsRequirements2KHR)
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance via xrCreateVulkanInstanceKHR — the runtime appends
// the instance extensions it needs.
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device selected by the runtime (xrGetVulkanGraphicsDevice2KHR)
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// Find a graphics queue family
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device via xrCreateVulkanDeviceKHR — the runtime
// appends its required device extensions and features (present_id/present_wait,
// timelineSemaphore); only the app's 3DGS features are passed through.
bool CreateVulkanDevice(XrSessionManager& xr, VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    VkDevice& device, VkQueue& graphicsQueue);

// Create OpenXR session with Vulkan binding + win32_window_binding
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd);
