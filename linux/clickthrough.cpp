// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 XShape click-through from the avatar silhouette (runtime#757).
 *
 * Ported from windows/main.cpp UpdateSilhouette + UpdateClickRegion (which use
 * SetWindowRgn); the X11 equivalent is an XShape ShapeInput region.
 */

#include "clickthrough.h"

#include "model_renderer.h"
#include "model_vulkan_utils.h"

#include <X11/extensions/shape.h>

#include <vector>
#include <cstring>
#include <cstdio>

// Scratch GPU image + host-visible readback for the silhouette pass (lazily
// created, resized with the window). Single-threaded — driven from the frame
// loop right after the eye render.
static ModelImage g_silImage = {};
static ModelBuffer g_silReadback = {};
static void* g_silMapped = nullptr;
static VkCommandPool g_pool = VK_NULL_HANDLE;

static bool EnsureTargets(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h)
{
	if (g_silImage.image != VK_NULL_HANDLE && g_silImage.width == w && g_silImage.height == h) {
		return true;
	}
	if (g_silImage.image != VK_NULL_HANDLE) {
		modelDestroyImage(dev, g_silImage);
	}
	if (g_silReadback.buffer != VK_NULL_HANDLE) {
		if (g_silMapped) {
			vkUnmapMemory(dev, g_silReadback.memory);
			g_silMapped = nullptr;
		}
		modelDestroyBuffer(dev, g_silReadback);
	}

	// COLOR_ATTACHMENT so renderEye can draw into it; TRANSFER_SRC to copy out.
	g_silImage = modelCreateImage2D(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM,
	                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (g_silImage.image == VK_NULL_HANDLE) {
		return false;
	}
	g_silReadback = modelCreateBuffer(dev, phys, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (g_silReadback.buffer == VK_NULL_HANDLE) {
		return false;
	}
	if (vkMapMemory(dev, g_silReadback.memory, 0, (VkDeviceSize)w * h * 4, 0, &g_silMapped) != VK_SUCCESS) {
		g_silMapped = nullptr;
		return false;
	}
	return g_silMapped != nullptr;
}

void
ClickthroughUpdate(VkDevice dev,
                   VkPhysicalDevice phys,
                   VkQueue queue,
                   uint32_t queueFamily,
                   ModelRenderer& renderer,
                   Display* dpy,
                   Window win,
                   uint32_t winW,
                   uint32_t winH,
                   const float viewMat[16],
                   const float projMat[16])
{
	if (dpy == nullptr || win == 0 || winW == 0 || winH == 0 || !renderer.hasModel()) {
		return;
	}

	// Downscale the silhouette to ~1/3 the window (cheap; the region is coarse).
	uint32_t w = winW / 3;
	if (w < 64) w = 64;
	if (w > 640) w = 640;
	uint32_t h = winH / 3;
	if (h < 64) h = 64;
	if (h > 360) h = 360;
	if (!EnsureTargets(dev, phys, w, h)) {
		return;
	}

	if (g_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = queueFamily;
		if (vkCreateCommandPool(dev, &pci, nullptr, &g_pool) != VK_SUCCESS) {
			return;
		}
	}

	// Render the avatar mono into the scratch (transparent bg → alpha≈1 on the
	// avatar, 0 on the background). renderEye leaves it in COLOR_ATTACHMENT_OPTIMAL.
	renderer.renderEye(g_silImage.image, VK_FORMAT_R8G8B8A8_UNORM, w, h, 0, 0, w, h, viewMat, projMat,
	                   /*transparentBg=*/true);

	// Copy the scratch alpha to the host buffer (blocking — coarse + downscaled).
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = g_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) {
		return;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	VkImageMemoryBarrier toSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toSrc.image = g_silImage.image;
	toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &toSrc);
	VkBufferImageCopy region = {};
	region.bufferRowLength = w;
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(cmd, g_silImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_silReadback.buffer, 1,
	                       &region);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);
	vkFreeCommandBuffers(dev, g_pool, 1, &cmd);

	const uint8_t* px = (const uint8_t*)g_silMapped;
	if (px == nullptr) {
		return;
	}

	// Build per-scanline input-region rects (alpha > 40 = avatar), scaled from
	// coverage space back up to window space.
	std::vector<XRectangle> rects;
	const float sx = (float)winW / (float)w;
	const float sy = (float)winH / (float)h;
	for (uint32_t cy = 0; cy < h; ++cy) {
		uint32_t cx = 0;
		while (cx < w) {
			if (px[(cy * w + cx) * 4 + 3] > 40) {
				uint32_t start = cx;
				while (cx < w && px[(cy * w + cx) * 4 + 3] > 40) {
					++cx;
				}
				XRectangle r;
				r.x = (short)(start * sx);
				r.y = (short)(cy * sy);
				r.width = (unsigned short)((cx - start) * sx + 1.0f);
				r.height = (unsigned short)(sy + 1.0f);
				rects.push_back(r);
			} else {
				++cx;
			}
		}
	}

	// Diagnostic (runtime#757 FLAG 1): the rect count disambiguates "region
	// empty" (silhouette produced no alpha → render/matrix issue) from "region
	// present but clicks still fall through" (coord/application issue). Throttled
	// ~every 60 updates (≈15 s at the 4 Hz update rate) so it's not spammy.
	static int s_diag = 0;
	if ((s_diag++ % 60) == 0) {
		uint32_t covered = 0;
		for (const auto& r : rects) covered += (uint32_t)r.width * r.height;
		fprintf(stderr,
		        "[INFO]  clickthrough: %zu input-rects, ~%u px covered, win %ux%u, coverage %ux%u\n",
		        rects.size(), covered, winW, winH, w, h);
	}

	// Set the window's INPUT shape to just the avatar: clicks land on it, the
	// transparent rest passes through to the desktop. Empty region = fully
	// click-through (no avatar this frame).
	XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, rects.empty() ? nullptr : rects.data(), (int)rects.size(),
	                        ShapeSet, Unsorted);
	XFlush(dpy);
}

void
ClickthroughDestroy(VkDevice dev)
{
	if (g_silMapped) {
		vkUnmapMemory(dev, g_silReadback.memory);
		g_silMapped = nullptr;
	}
	if (g_silReadback.buffer != VK_NULL_HANDLE) {
		modelDestroyBuffer(dev, g_silReadback);
	}
	if (g_silImage.image != VK_NULL_HANDLE) {
		modelDestroyImage(dev, g_silImage);
	}
	if (g_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(dev, g_pool, nullptr);
		g_pool = VK_NULL_HANDLE;
	}
}
