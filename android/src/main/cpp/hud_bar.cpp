// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// CPU-rasterized button bar uploaded into an OpenXR swapchain — see hud_bar.h.

#include "hud_bar.h"

#include <android/log.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "stb_truetype.h"

#define LOG_TAG "model_viewer_vk_android"
#define BAR_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define BAR_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kFirstChar = 32;
constexpr int kNumChars = 95;  // ASCII 32..126

// System TTFs to try, in order. DroidSansMono is the cube_handle_vk_android
// precedent; Roboto ships on every Android image.
const char *const kFontPaths[] = {
    "/system/fonts/RobotoCondensed-Regular.ttf",
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/DroidSansMono.ttf",
};

bool
read_file(const char *path, std::vector<unsigned char> &out)
{
	FILE *f = std::fopen(path, "rb");
	if (f == nullptr) {
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (len <= 0) {
		std::fclose(f);
		return false;
	}
	out.resize((size_t)len);
	bool ok = std::fread(out.data(), 1, (size_t)len, f) == (size_t)len;
	std::fclose(f);
	return ok;
}

uint32_t
find_host_visible_mem_type(VkPhysicalDevice phys, uint32_t type_bits)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	const VkMemoryPropertyFlags want =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

const stbtt_bakedchar *
baked(const HudBar &bar)
{
	return reinterpret_cast<const stbtt_bakedchar *>(bar.baked_chars.data());
}

float
text_width_px(const HudBar &bar, const char *text)
{
	float w = 0.0f;
	for (const char *p = text; *p != '\0'; ++p) {
		int c = (unsigned char)*p;
		if (c < kFirstChar || c >= kFirstChar + kNumChars) {
			c = '?';
		}
		w += baked(bar)[c - kFirstChar].xadvance;
	}
	return w;
}

// Signed distance to a rounded rect centered box; negative inside.
float
rounded_rect_dist(float px, float py, float cx, float cy, float hw, float hh, float r)
{
	const float qx = std::fabs(px - cx) - (hw - r);
	const float qy = std::fabs(py - cy) - (hh - r);
	const float ax = qx > 0.0f ? qx : 0.0f;
	const float ay = qy > 0.0f ? qy : 0.0f;
	const float outside = std::sqrt(ax * ax + ay * ay);
	const float inside = (qx > qy ? qx : qy) < 0.0f ? (qx > qy ? qx : qy) : 0.0f;
	return outside + inside - r;
}

// Straight-alpha source-over into the RGBA8 compose buffer.
void
blend_px(unsigned char *dst, float r, float g, float b, float a)
{
	if (a <= 0.0f) {
		return;
	}
	const float da = dst[3] / 255.0f;
	const float oa = a + da * (1.0f - a);
	if (oa <= 0.0f) {
		dst[0] = dst[1] = dst[2] = dst[3] = 0;
		return;
	}
	const float inv = 1.0f - a;
	dst[0] = (unsigned char)((r * a + (dst[0] / 255.0f) * da * inv) / oa * 255.0f + 0.5f);
	dst[1] = (unsigned char)((g * a + (dst[1] / 255.0f) * da * inv) / oa * 255.0f + 0.5f);
	dst[2] = (unsigned char)((b * a + (dst[2] / 255.0f) * da * inv) / oa * 255.0f + 0.5f);
	dst[3] = (unsigned char)(oa * 255.0f + 0.5f);
}

void
draw_text(HudBar &bar, const char *text, float origin_x, float baseline_y, float clip_x0, float clip_x1)
{
	float pen_x = origin_x;
	for (const char *p = text; *p != '\0'; ++p) {
		int c = (unsigned char)*p;
		if (c < kFirstChar || c >= kFirstChar + kNumChars) {
			c = '?';
		}
		const stbtt_bakedchar &bc = baked(bar)[c - kFirstChar];
		const int gw = bc.x1 - bc.x0;
		const int gh = bc.y1 - bc.y0;
		const int dx0 = (int)std::lround(pen_x + bc.xoff);
		const int dy0 = (int)std::lround(baseline_y + bc.yoff);
		for (int gy = 0; gy < gh; ++gy) {
			const int ty = dy0 + gy;
			if (ty < 0 || ty >= (int)bar.tex_h) {
				continue;
			}
			for (int gx = 0; gx < gw; ++gx) {
				const int tx = dx0 + gx;
				if (tx < (int)clip_x0 || tx >= (int)clip_x1 || tx < 0 || tx >= (int)bar.tex_w) {
					continue;
				}
				const float cov =
				    bar.font_atlas[(size_t)(bc.y0 + gy) * bar.atlas_w + (bc.x0 + gx)] / 255.0f;
				blend_px(&bar.pixels[((size_t)ty * bar.tex_w + tx) * 4], 1.0f, 1.0f, 1.0f, cov);
			}
		}
		pen_x += bc.xadvance;
	}
}

} // namespace

bool
hud_bar_init(HudBar &bar,
             XrSession session,
             VkPhysicalDevice phys,
             VkDevice device,
             VkQueue queue,
             uint32_t queue_family,
             VkFormat format,
             uint32_t tex_w,
             uint32_t tex_h)
{
	bar.device = device;
	bar.queue = queue;
	bar.tex_w = tex_w;
	bar.tex_h = tex_h;

	// Bake the font (R8 coverage atlas).
	std::vector<unsigned char> ttf;
	const char *used = nullptr;
	for (const char *path : kFontPaths) {
		if (read_file(path, ttf)) {
			used = path;
			break;
		}
	}
	if (used == nullptr) {
		BAR_LOGW("hud_bar: no system TTF found — button UI disabled");
		return false;
	}
	bar.font_px = (float)tex_h * 0.42f;
	bar.atlas_w = 1024;
	bar.atlas_h = 256;
	bar.font_atlas.assign((size_t)bar.atlas_w * bar.atlas_h, 0);
	bar.baked_chars.assign(sizeof(stbtt_bakedchar) * kNumChars, 0);
	if (stbtt_BakeFontBitmap(ttf.data(), 0, bar.font_px, bar.font_atlas.data(), bar.atlas_w,
	                         bar.atlas_h, kFirstChar, kNumChars,
	                         reinterpret_cast<stbtt_bakedchar *>(bar.baked_chars.data())) <= 0) {
		BAR_LOGW("hud_bar: stbtt_BakeFontBitmap failed for %s", used);
		return false;
	}

	// HUD swapchain — same RGBA8 format the model swapchains use.
	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = (int64_t)format;
	ci.sampleCount = 1;
	ci.width = tex_w;
	ci.height = tex_h;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	if (xrCreateSwapchain(session, &ci, &bar.swapchain) != XR_SUCCESS) {
		BAR_LOGW("hud_bar: xrCreateSwapchain failed — button UI disabled");
		return false;
	}
	uint32_t img_count = 0;
	xrEnumerateSwapchainImages(bar.swapchain, 0, &img_count, nullptr);
	if (img_count > 8) {
		img_count = 8;
	}
	for (uint32_t i = 0; i < img_count; ++i) {
		bar.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	if (xrEnumerateSwapchainImages(bar.swapchain, img_count, &img_count,
	                               reinterpret_cast<XrSwapchainImageBaseHeader *>(bar.images)) !=
	    XR_SUCCESS) {
		BAR_LOGW("hud_bar: xrEnumerateSwapchainImages failed");
		hud_bar_destroy(bar);
		return false;
	}
	bar.image_count = img_count;

	// Persistent host-visible staging buffer + command pool.
	const VkDeviceSize staging_size = (VkDeviceSize)tex_w * tex_h * 4;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = staging_size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &bi, nullptr, &bar.staging) != VK_SUCCESS) {
		hud_bar_destroy(bar);
		return false;
	}
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(device, bar.staging, &mr);
	const uint32_t mt = find_host_visible_mem_type(phys, mr.memoryTypeBits);
	if (mt == UINT32_MAX) {
		hud_bar_destroy(bar);
		return false;
	}
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mt;
	if (vkAllocateMemory(device, &ai, nullptr, &bar.staging_mem) != VK_SUCCESS ||
	    vkBindBufferMemory(device, bar.staging, bar.staging_mem, 0) != VK_SUCCESS ||
	    vkMapMemory(device, bar.staging_mem, 0, staging_size, 0, &bar.staging_mapped) != VK_SUCCESS) {
		hud_bar_destroy(bar);
		return false;
	}
	VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pi.queueFamilyIndex = queue_family;
	if (vkCreateCommandPool(device, &pi, nullptr, &bar.cmd_pool) != VK_SUCCESS) {
		hud_bar_destroy(bar);
		return false;
	}

	bar.pixels.assign((size_t)tex_w * tex_h * 4, 0);
	bar.ready = true;
	BAR_LOGI("hud_bar: ready (%ux%u, font %s @ %.0fpx, %u images)", tex_w, tex_h, used,
	         bar.font_px, bar.image_count);
	return true;
}

void
hud_bar_destroy(HudBar &bar)
{
	if (bar.device != VK_NULL_HANDLE) {
		if (bar.cmd_pool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(bar.device, bar.cmd_pool, nullptr);
			bar.cmd_pool = VK_NULL_HANDLE;
		}
		if (bar.staging_mapped != nullptr) {
			vkUnmapMemory(bar.device, bar.staging_mem);
			bar.staging_mapped = nullptr;
		}
		if (bar.staging != VK_NULL_HANDLE) {
			vkDestroyBuffer(bar.device, bar.staging, nullptr);
			bar.staging = VK_NULL_HANDLE;
		}
		if (bar.staging_mem != VK_NULL_HANDLE) {
			vkFreeMemory(bar.device, bar.staging_mem, nullptr);
			bar.staging_mem = VK_NULL_HANDLE;
		}
	}
	if (bar.swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(bar.swapchain);
		bar.swapchain = XR_NULL_HANDLE;
	}
	bar.ready = false;
}

void
hud_bar_render(HudBar &bar, const HudBarButton *buttons, uint32_t count)
{
	if (!bar.ready) {
		return;
	}
	std::memset(bar.pixels.data(), 0, bar.pixels.size());

	// Pills fill ~70% of the bar height, vertically centered (Windows-leg look).
	const float pill_y0 = bar.tex_h * 0.15f;
	const float pill_y1 = bar.tex_h * 0.85f;
	const float cy = 0.5f * (pill_y0 + pill_y1);
	const float hh = 0.5f * (pill_y1 - pill_y0);
	const float radius = hh * 0.45f;

	for (uint32_t b = 0; b < count; ++b) {
		if (!buttons[b].enabled) {
			continue;
		}
		// The layer spans the full window width, so window-x fractions map
		// straight onto bar-texture x.
		const float x0 = buttons[b].x_frac * bar.tex_w;
		const float x1 = (buttons[b].x_frac + buttons[b].w_frac) * bar.tex_w;
		const float cx = 0.5f * (x0 + x1);
		const float hw = 0.5f * (x1 - x0);

		const int ix0 = (int)std::floor(x0) < 0 ? 0 : (int)std::floor(x0);
		const int ix1 = (int)std::ceil(x1) > (int)bar.tex_w ? (int)bar.tex_w : (int)std::ceil(x1);
		for (int y = (int)pill_y0; y < (int)std::ceil(pill_y1); ++y) {
			for (int x = ix0; x < ix1; ++x) {
				const float d =
				    rounded_rect_dist((float)x + 0.5f, (float)y + 0.5f, cx, cy, hw, hh, radius);
				// 1px antialiased edge; dark translucent fill + lighter rim.
				const float fill_cov = d < -1.0f ? 1.0f : (d < 0.0f ? -d : 0.0f);
				const float rim_cov = std::fabs(d + 0.75f) < 1.25f ? 1.0f - std::fabs(d + 0.75f) / 1.25f : 0.0f;
				unsigned char *dst = &bar.pixels[((size_t)y * bar.tex_w + x) * 4];
				blend_px(dst, 0.10f, 0.11f, 0.13f, fill_cov * 0.82f);
				blend_px(dst, 0.55f, 0.58f, 0.64f, rim_cov * 0.55f);
			}
		}

		// Centered label, clipped to the pill interior.
		const float tw = text_width_px(bar, buttons[b].label.c_str());
		const float pad = radius;
		float tx = cx - 0.5f * tw;
		if (tx < x0 + pad) {
			tx = x0 + pad;
		}
		const float baseline = cy + bar.font_px * 0.34f;
		draw_text(bar, buttons[b].label.c_str(), tx, baseline, x0 + pad * 0.5f, x1 - pad * 0.5f);
	}
}

bool
hud_bar_upload(HudBar &bar, float alpha)
{
	if (!bar.ready) {
		return false;
	}
	if (alpha < 0.0f) {
		alpha = 0.0f;
	}
	if (alpha > 1.0f) {
		alpha = 1.0f;
	}

	// Stage with the global fade applied to the alpha channel (straight alpha).
	const size_t n = (size_t)bar.tex_w * bar.tex_h;
	const unsigned char *src = bar.pixels.data();
	unsigned char *dst = (unsigned char *)bar.staging_mapped;
	if (alpha >= 1.0f) {
		std::memcpy(dst, src, n * 4);
	} else {
		const uint32_t a8 = (uint32_t)(alpha * 256.0f + 0.5f);
		for (size_t i = 0; i < n; ++i) {
			dst[i * 4 + 0] = src[i * 4 + 0];
			dst[i * 4 + 1] = src[i * 4 + 1];
			dst[i * 4 + 2] = src[i * 4 + 2];
			dst[i * 4 + 3] = (unsigned char)((src[i * 4 + 3] * a8) >> 8);
		}
	}

	XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	uint32_t idx = 0;
	if (xrAcquireSwapchainImage(bar.swapchain, &acq, &idx) != XR_SUCCESS) {
		return false;
	}
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = XR_INFINITE_DURATION;
	if (xrWaitSwapchainImage(bar.swapchain, &wi) != XR_SUCCESS) {
		return false;
	}

	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = bar.cmd_pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(bar.device, &cai, &cb) != VK_SUCCESS) {
		XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImage(bar.swapchain, &rel);
		return false;
	}
	VkCommandBufferBeginInfo bgi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &bgi);

	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = 0;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = bar.images[idx].image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);

	VkBufferImageCopy rg = {};
	rg.bufferRowLength = bar.tex_w;
	rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	rg.imageExtent = {bar.tex_w, bar.tex_h, 1};
	vkCmdCopyBufferToImage(cb, bar.staging, bar.images[idx].image,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);

	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &b);

	vkEndCommandBuffer(cb);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	vkQueueSubmit(bar.queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(bar.queue);
	vkFreeCommandBuffers(bar.device, bar.cmd_pool, 1, &cb);

	XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	return xrReleaseSwapchainImage(bar.swapchain, &rel) == XR_SUCCESS;
}
