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

// XR_DXR_display_zones entry points (resolved in InitializeOpenXR; NULL when
// the extension is absent — callers must check).
extern PFN_xrGetDisplayZoneCapabilitiesDXR g_pfnGetDisplayZoneCaps;
extern PFN_xrGetDisplayZoneRecommendedViewSizeDXR g_pfnGetDisplayZoneViewSize;

// 3D-panel top-left in virtual-desktop pixels (XrDisplayDesktopPositionDXR,
// XR_DXR_display_info v16, runtime#715), captured in InitializeOpenXR. (0,0)
// = primary monitor or unknown (older runtime) — the safe default.
extern int32_t g_displayDesktopLeft;
extern int32_t g_displayDesktopTop;

// Initialize OpenXR instance with Vulkan + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get Vulkan graphics requirements and set up Vulkan instance/device per OpenXR spec
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance with required extensions from the runtime
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device selected by the runtime
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// Get required device extensions from the runtime
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage);

// Find a graphics queue family
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device with required extensions
bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue);

// Create OpenXR session with Vulkan binding + win32_window_binding
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd);
