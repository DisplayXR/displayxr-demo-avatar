// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// On-screen button bar for the Android leg, drawn into an XR_EXT window-space
// composition layer (the same single full-width top-bar model as the Windows
// leg, windows/main.cpp "Top button bar"). The bar texture is rasterized on
// the CPU (stb_truetype glyphs from a system TTF + rounded pills — no
// platform font API, mirrors cube_handle_vk_android's hud_font approach) and
// uploaded into an OpenXR swapchain via a persistent staging buffer (the
// windowspace_handle_vk_win pattern). Straight (non-premultiplied) alpha:
// the layer is submitted with XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA
// and comp_multi blends srcColor=SRC_ALPHA, so a global fade just scales the
// alpha channel at upload.

#pragma once

#include <vulkan/vulkan.h>

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>  // XrSwapchainImageVulkanKHR

#include <string>
#include <vector>

struct HudBarButton
{
	float x_frac;       // left edge, fraction of the WINDOW width (== bar-texture x)
	float w_frac;       // width, fraction of the window width
	std::string label;  // pill text (clipped to the pill)
	bool enabled;       // drawn when true
};

struct HudBar
{
	bool ready = false;

	// OpenXR swapchain (RGBA8) the compositor samples for the layer.
	XrSwapchain swapchain = XR_NULL_HANDLE;
	uint32_t tex_w = 0;
	uint32_t tex_h = 0;
	XrSwapchainImageVulkanKHR images[8] = {};
	uint32_t image_count = 0;

	// Upload plumbing (persistent staging + transient command buffers).
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	void *staging_mapped = nullptr;
	VkCommandPool cmd_pool = VK_NULL_HANDLE;

	// stb_truetype glyph atlas (R8 coverage), baked once at init.
	std::vector<unsigned char> font_atlas;
	int atlas_w = 0;
	int atlas_h = 0;
	float font_px = 0.0f;
	// stbtt_bakedchar for ASCII 32..126; stored as raw bytes so this header
	// doesn't need stb_truetype.h.
	std::vector<unsigned char> baked_chars;

	// CPU compose buffer: the bar at alpha=1 (straight-alpha RGBA8).
	std::vector<unsigned char> pixels;
};

// Create the swapchain (tex_w x tex_h, the session's RGBA8 format), staging
// buffer + command pool, and bake the system font. Non-fatal on failure
// (returns false, bar stays !ready and the app simply has no button UI).
bool
hud_bar_init(HudBar &bar,
             XrSession session,
             VkPhysicalDevice phys,
             VkDevice device,
             VkQueue queue,
             uint32_t queue_family,
             VkFormat format,
             uint32_t tex_w,
             uint32_t tex_h);

void
hud_bar_destroy(HudBar &bar);

// (Re)rasterize the base pixels: transparent bar, one rounded pill + centered
// label per enabled button.
void
hud_bar_render(HudBar &bar, const HudBarButton *buttons, uint32_t count);

// Upload the base pixels, with the alpha channel scaled by `alpha` [0..1],
// into the next swapchain image (acquire → copy → release). The released
// image keeps feeding the layer until the next upload, so call this only when
// content or alpha changed.
bool
hud_bar_upload(HudBar &bar, float alpha);
