// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 XShape click-through from the avatar silhouette (runtime#757).
 *
 * Ported from windows/main.cpp UpdateSilhouette + UpdateClickRegion (which use
 * SetWindowRgn); the X11 equivalent is an XShape ShapeInput region. See
 * clickthrough.h for what this port fixes relative to the first Linux cut.
 */

#include "clickthrough.h"

#include "model_renderer.h"
#include "model_vulkan_utils.h"

#include <X11/extensions/shape.h>

#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ── Tunables, matching the Windows leg's env levers ─────────────────────────
// DXR_AVATAR_SIL_TEXEL_PX  window px per coverage texel (default 4)
// DXR_AVATAR_SIL_DILATE    dilation radius in texels    (default 1)
// DXR_AVATAR_SIL_ALPHA     coverage alpha threshold     (default 8)
// DXR_VK_NO_HOST_CACHED=1  force the write-combined readback (repro lever)
static constexpr uint32_t kSilMaxW = 1024, kSilMaxH = 576;
static constexpr uint32_t kSilMinW = 64, kSilMinH = 64;
static constexpr uint32_t kTexelPxDefault = 4;

static uint32_t
SilEnvUInt(const char *name, uint32_t def, uint32_t lo, uint32_t hi)
{
	const char *e = getenv(name);
	if (e == nullptr || e[0] == '\0') {
		return def;
	}
	const long v = strtol(e, nullptr, 10);
	if (v < (long)lo || v > (long)hi) {
		return def;
	}
	return (uint32_t)v;
}

// Scratch GPU images + host-visible readback for the silhouette pass (lazily
// created, resized with the window). TWO images: the coverage is the union of
// the outermost views and both halves must come from the SAME frame, or the
// union is stale in one eye. Single-threaded — driven from the frame loop.
static ModelImage g_silImage = {};  // first view
static ModelImage g_silImage2 = {}; // last view (stereo only)
static ModelBuffer g_silReadback = {};
static void *g_silMapped = nullptr;
static VkCommandPool g_pool = VK_NULL_HANDLE;

// Pipelined readback state (#837 on the Windows leg): each invocation submits
// its renders + copies behind a fence and does NOT wait; the NEXT invocation
// consumes the bytes. Staleness is exactly one frame — against the ~16 ms
// synchronous stall the old vkQueueWaitIdle cost, which is what forced the
// 4 Hz throttle.
static VkFence g_silFence = VK_NULL_HANDLE;
static bool g_silPending = false;
static uint32_t g_silPendingW = 0, g_silPendingH = 0;
static bool g_silPendingTwo = false;
static uint32_t g_silPendingY0 = 0;   // first raster row the avatar was drawn into
static VkCommandBuffer g_silPrevCmd = VK_NULL_HANDLE;

// Published coverage: one byte per texel, 1 = avatar present.
static std::vector<uint8_t> g_covBits;
static uint32_t g_covW = 0, g_covH = 0;
// The first row that was actually rendered. Rows above it are untouched scratch
// and must be skipped — derived from the SAME avatarY the render used, not
// recomputed from avatarFrac, because rounding the two independently can differ
// by a row and expose undefined texels.
static uint32_t g_covY0 = 0;
static bool g_covReady = false;

static bool
EnsureTargets(VkDevice dev, VkPhysicalDevice phys, VkQueue queue, VkCommandPool pool, uint32_t w, uint32_t h)
{
	// Every handle, not just the first image: a partial failure below used to
	// leave g_silImage valid and the rest null, and the NEXT call would
	// short-circuit to "ready" and blit into VK_NULL_HANDLE. An allocation
	// failure should degrade to "no click-through", never to a crash.
	if (g_silImage.image != VK_NULL_HANDLE && g_silImage2.image != VK_NULL_HANDLE &&
	    g_silReadback.buffer != VK_NULL_HANDLE && g_silMapped != nullptr && g_silImage.width == w &&
	    g_silImage.height == h) {
		return true;
	}
	if (g_silImage.image != VK_NULL_HANDLE) {
		modelDestroyImage(dev, g_silImage);
	}
	if (g_silImage2.image != VK_NULL_HANDLE) {
		modelDestroyImage(dev, g_silImage2);
	}
	if (g_silReadback.buffer != VK_NULL_HANDLE) {
		if (g_silMapped) {
			vkUnmapMemory(dev, g_silReadback.memory);
			g_silMapped = nullptr;
		}
		modelDestroyBuffer(dev, g_silReadback);
	}

	// All three usages are load-bearing, and dropping any one is a spec
	// violation the drivers we happen to run on tolerate:
	//   TRANSFER_DST  — ModelRenderer::renderEye BLITS into its target (it
	//                   renders to its own MSAA targets and resolves out).
	//                   The pre-port set omitted this.
	//   TRANSFER_SRC  — the copy-out reads it.
	//   COLOR_ATTACHMENT — renderEye leaves the target in
	//                   COLOR_ATTACHMENT_OPTIMAL and declares that layout on
	//                   entry, which requires the usage
	//                   (VUID-VkImageMemoryBarrier-oldLayout-01197); and
	//                   modelCreateImage2D unconditionally creates a
	//                   VkImageView, which a TRANSFER-only image is not
	//                   compatible with (VUID-VkImageViewCreateInfo-image-04441).
	//                   windows/main.cpp's EnsureSilhouetteTargets has the same
	//                   gap and silently logs "failed to create image view".
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	g_silImage = modelCreateImage2D(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, usage);
	if (g_silImage.image == VK_NULL_HANDLE) {
		return false;
	}
	g_silImage2 = modelCreateImage2D(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, usage);
	if (g_silImage2.image == VK_NULL_HANDLE) {
		return false;
	}

	// HOST_CACHED: this buffer is READ back every frame, and
	// HOST_VISIBLE|HOST_COHERENT alone is typically WRITE-COMBINED on a
	// discrete GPU — fast to write, pathologically slow to read. Measured on
	// the Windows leg (RTX 3080): ~127 ms/frame write-combined against
	// ~0.86 ms cached. That cost is what made this pass look too expensive to
	// run at frame rate. DXR_VK_NO_HOST_CACHED=1 forces the legacy allocation
	// so the pathology stays reproducible.
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 4 * 2;
	static const bool s_noCached = getenv("DXR_VK_NO_HOST_CACHED") != nullptr;
	g_silReadback = modelCreateBuffer(
	    dev, phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    s_noCached ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	               : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
	if (g_silReadback.buffer == VK_NULL_HANDLE) {
		fprintf(stderr, "[WARN]  clickthrough: no HOST_CACHED memory type — using write-combined; "
		                "the per-frame coverage read will be slower on this driver.\n");
		g_silReadback = modelCreateBuffer(dev, phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
	if (g_silReadback.buffer == VK_NULL_HANDLE) {
		return false;
	}
	if (vkMapMemory(dev, g_silReadback.memory, 0, bytes, 0, &g_silMapped) != VK_SUCCESS) {
		g_silMapped = nullptr;
		return false;
	}

	// Put both images into COLOR_ATTACHMENT_OPTIMAL before first use.
	// renderEye picks its entry layout from
	// `firstViewInImage = (viewportX == 0 && viewportY == 0)`, and the
	// silhouette pass renders at a non-zero viewportY (the avatar sits in the
	// bottom band), so it ALWAYS declares COLOR_ATTACHMENT_OPTIMAL — never
	// UNDEFINED. Freshly created images are UNDEFINED, so without this the very
	// first frame's barrier lies about the layout. The copy-out below restores
	// the same layout on every subsequent frame.
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	VkImageMemoryBarrier init = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	init.srcAccessMask = 0;
	init.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	init.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	init.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	init.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	init.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	init.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier inits[2] = {init, init};
	inits[0].image = g_silImage.image;
	inits[1].image = g_silImage2.image;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 2, inits);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);   // once per (re)size only, not per frame
	vkFreeCommandBuffers(dev, pool, 1, &cmd);
	return true;
}

//! Is XShape present on this server? Queried once; a server without it gets a
//! single WARN rather than a silent no-op (the old code never checked).
static bool
HaveShapeExtension(Display *dpy)
{
	static int s_state = -1; // -1 unknown, 0 absent, 1 present
	if (s_state < 0) {
		int eventBase = 0, errorBase = 0;
		s_state = XShapeQueryExtension(dpy, &eventBase, &errorBase) ? 1 : 0;
		if (s_state == 0) {
			fprintf(stderr, "[WARN]  clickthrough: the X server has no SHAPE extension — "
			                "the overlay will swallow clicks over its whole window "
			                "instead of passing them through to the desktop.\n");
		}
	}
	return s_state == 1;
}

//! Fold the previous invocation's readback into the published coverage:
//! union both planes' alpha, threshold, dilate.
static void
ConsumePendingReadback(VkDevice dev, uint32_t dilate, uint8_t alphaMin)
{
	if (!g_silPending || g_silFence == VK_NULL_HANDLE) {
		return;
	}
	vkWaitForFences(dev, 1, &g_silFence, VK_TRUE, UINT64_MAX);
	g_silPending = false;

	const uint8_t *px = (const uint8_t *)g_silMapped;
	const uint32_t cw = g_silPendingW, ch = g_silPendingH;
	const size_t n = (size_t)cw * ch;
	if (px == nullptr || n == 0) {
		return;
	}

	// Row-wise memcpy BEFORE touching the bytes: memcpy uses wide streaming
	// loads, which survive write-combined memory; a per-texel loop does not.
	// Insurance for drivers with no HOST_CACHED type.
	std::vector<uint8_t> unionAlpha(n, 0);
	std::vector<uint8_t> rowBuf((size_t)cw * 4);
	for (uint32_t y = 0; y < ch; ++y) {
		uint8_t *out = unionAlpha.data() + (size_t)y * cw;
		memcpy(rowBuf.data(), px + (size_t)y * cw * 4, (size_t)cw * 4);
		for (uint32_t x = 0; x < cw; ++x) {
			out[x] = rowBuf[(size_t)x * 4 + 3];
		}
		if (g_silPendingTwo) {
			memcpy(rowBuf.data(), px + n * 4 + (size_t)y * cw * 4, (size_t)cw * 4);
			for (uint32_t x = 0; x < cw; ++x) {
				const uint8_t a = rowBuf[(size_t)x * 4 + 3];
				if (a > out[x]) {
					out[x] = a;
				}
			}
		}
	}

	std::vector<uint8_t> bits(n, 0);
	for (size_t i = 0; i < n; ++i) {
		bits[i] = (unionAlpha[i] > alphaMin) ? 1 : 0;
	}
	// Separable dilation, both passes biased outward: the region DELETES what
	// it does not cover, so an under-large mask loses pixels the user can see.
	if (dilate > 0) {
		const uint32_t R = dilate;
		std::vector<uint8_t> tmp(n, 0);
		for (uint32_t y = 0; y < ch; ++y) {
			const uint8_t *src = bits.data() + (size_t)y * cw;
			uint8_t *dst = tmp.data() + (size_t)y * cw;
			for (uint32_t x = 0; x < cw; ++x) {
				if (!src[x]) {
					continue;
				}
				const uint32_t x0 = (x > R) ? x - R : 0;
				const uint32_t x1 = (x + R + 1 < cw) ? x + R + 1 : cw;
				memset(dst + x0, 1, x1 - x0);
			}
		}
		std::vector<uint8_t> out(n, 0);
		for (uint32_t y = 0; y < ch; ++y) {
			const uint8_t *src = tmp.data() + (size_t)y * cw;
			const uint32_t y0 = (y > R) ? y - R : 0;
			const uint32_t y1 = (y + R + 1 < ch) ? y + R + 1 : ch;
			for (uint32_t yy = y0; yy < y1; ++yy) {
				uint8_t *d = out.data() + (size_t)yy * cw;
				for (uint32_t x = 0; x < cw; ++x) {
					d[x] |= src[x];
				}
			}
		}
		bits.swap(out);
	}

	g_covBits.swap(bits);
	g_covW = cw;
	g_covH = ch;
	g_covY0 = (g_silPendingY0 < ch) ? g_silPendingY0 : 0;
	g_covReady = true;
}

//! Turn the published coverage (+ the bubble rect) into the window's XShape
//! input region.
static void
ApplyRegion(const ClickthroughParams &p)
{
	if (!g_covReady || g_covW == 0 || g_covH == 0 || p.winW == 0 || p.winH == 0) {
		return;
	}
	std::vector<XRectangle> rects;

	// Scale against the LIVE window, not the size the coverage was captured at.
	// The coverage is a normalised silhouette, so mapping it onto the current
	// rect is the right thing during a resize — and it keeps the runs in the
	// same frame as the bubble rect unioned in below, which is always current.
	const int64_t cw = (int64_t)g_covW, ch = (int64_t)g_covH;
	const int64_t winW = (int64_t)p.winW, winH = (int64_t)p.winH;
	// Skip the rows above the rendered band: they are untouched scratch, and
	// the bubble rect below covers that area explicitly.
	int64_t yStart = (int64_t)g_covY0;
	if (yStart < 0) {
		yStart = 0;
	}
	if (yStart > ch) {
		yStart = ch;
	}

	// One client-space rect per horizontal run, with identical consecutive
	// rows folded into a single band — the raster now scales with the window
	// (540 rows on a 4K panel) and would otherwise hand the server thousands
	// of rectangles per frame for a shape that is mostly vertical edges.
	std::vector<int> runs, prevRuns;
	size_t bandFirst = (size_t)-1;
	for (int64_t y = yStart; y < ch; ++y) {
		runs.clear();
		int64_t x = 0;
		while (x < cw) {
			if (!g_covBits[(size_t)(y * cw + x)]) {
				++x;
				continue;
			}
			const int64_t xs = x;
			while (x < cw && g_covBits[(size_t)(y * cw + x)]) {
				++x;
			}
			runs.push_back((int)xs);
			runs.push_back((int)x);
		}
		const int64_t bottom = (y + 1) * winH / ch;
		if (runs.empty()) {
			bandFirst = (size_t)-1;
			prevRuns.clear();
			continue;
		}
		if (bandFirst != (size_t)-1 && runs == prevRuns) {
			for (size_t i = bandFirst; i < rects.size(); ++i) {
				rects[i].height = (unsigned short)(bottom - (int64_t)rects[i].y);
			}
			continue;
		}
		bandFirst = rects.size();
		const int64_t top = y * winH / ch;
		for (size_t i = 0; i + 1 < runs.size(); i += 2) {
			const int64_t left = (int64_t)runs[i] * winW / cw;
			const int64_t right = (int64_t)runs[i + 1] * winW / cw;
			XRectangle r;
			r.x = (short)left;
			r.y = (short)top;
			r.width = (unsigned short)(right > left ? right - left : 1);
			r.height = (unsigned short)(bottom > top ? bottom - top : 1);
			rects.push_back(r);
		}
		prevRuns = runs;
	}

	// The bubble sits above the avatar, outside its silhouette — union it in or
	// the shaped window makes it unclickable (and, on a shaped window, the
	// region is a VISUAL clip too).
	if (p.bubbleVisible && p.bubbleW > 0 && p.bubbleH > 0) {
		XRectangle r;
		r.x = (short)p.bubbleX;
		r.y = (short)p.bubbleY;
		r.width = (unsigned short)p.bubbleW;
		r.height = (unsigned short)p.bubbleH;
		rects.push_back(r);
	}

	// Diagnostic (runtime#757 FLAG 1): the rect count disambiguates "region
	// empty" (no alpha → render/matrix issue) from "region present but clicks
	// still fall through" (coord/application issue). Throttled to ~once every
	// 300 updates (≈5 s at 60 Hz) now that this runs every frame.
	static int s_diag = 0;
	if ((s_diag++ % 300) == 0) {
		uint32_t covered = 0;
		for (const auto &r : rects) {
			covered += (uint32_t)r.width * r.height;
		}
		fprintf(stderr,
		        "[INFO]  clickthrough: %zu input-rects, ~%u px covered, win %lldx%lld, coverage %ux%u\n",
		        rects.size(), covered, (long long)winW, (long long)winH, g_covW, g_covH);
	}

	// Set the window's INPUT shape to just the avatar (+ bubble): clicks land
	// on it, the transparent rest passes through to the desktop. An empty
	// region means fully click-through (nothing drawn this frame).
	XShapeCombineRectangles(p.dpy, p.win, ShapeInput, 0, 0, rects.empty() ? nullptr : rects.data(),
	                        (int)rects.size(), ShapeSet, Unsorted);
	XFlush(p.dpy);
}

void
ClickthroughUpdate(const ClickthroughParams &p)
{
	if (p.dpy == nullptr || p.win == 0 || p.winW == 0 || p.winH == 0 || p.renderer == nullptr ||
	    !p.renderer->hasModel() || p.numViews == 0 || p.viewMats == nullptr || p.projMats == nullptr) {
		return;
	}
	if (!HaveShapeExtension(p.dpy)) {
		return;
	}

	// Decorated (B) or opaque (Ctrl+T): the whole window is interactive. A WM
	// frame needs it for move/resize, and an opaque frame covers every pixel,
	// so shaping it to the silhouette would clip the background it just drew.
	static bool s_shapeCleared = false;
	if (p.decorated || !p.transparentBg) {
		if (!s_shapeCleared) {
			XShapeCombineMask(p.dpy, p.win, ShapeInput, 0, 0, None, ShapeSet);
			XFlush(p.dpy);
			s_shapeCleared = true;
		}
		return;
	}
	// Back to borderless + transparent: the region is re-applied below. The
	// latch must be cleared here, not only set above, or a second B press
	// would find s_shapeCleared already true and never drop the shape again.
	s_shapeCleared = false;

	static const uint32_t s_texelPx = SilEnvUInt("DXR_AVATAR_SIL_TEXEL_PX", kTexelPxDefault, 1, 64);
	static const uint32_t s_dilate = SilEnvUInt("DXR_AVATAR_SIL_DILATE", 1, 0, 16);
	static const uint8_t s_alphaMin = (uint8_t)SilEnvUInt("DXR_AVATAR_SIL_ALPHA", 8, 1, 255);

	uint32_t w = (p.winW + s_texelPx - 1) / s_texelPx;
	uint32_t h = (p.winH + s_texelPx - 1) / s_texelPx;
	if (w < kSilMinW) {
		w = kSilMinW;
	}
	if (w > kSilMaxW) {
		w = kSilMaxW;
	}
	if (h < kSilMinH) {
		h = kSilMinH;
	}
	if (h > kSilMaxH) {
		h = kSilMaxH;
	}

	// 1. Consume the previous invocation's readback BEFORE EnsureTargets may
	//    recreate the image/buffer under an in-flight copy.
	ConsumePendingReadback(p.dev, s_dilate, s_alphaMin);

	// The pool must exist before EnsureTargets — it submits the images' initial
	// layout transition.
	if (g_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = p.queueFamily;
		if (vkCreateCommandPool(p.dev, &pci, nullptr, &g_pool) != VK_SUCCESS) {
			return;
		}
	}
	if (!EnsureTargets(p.dev, p.phys, p.queue, g_pool, w, h)) {
		return;
	}
	if (g_silFence == VK_NULL_HANDLE) {
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(p.dev, &fci, nullptr, &g_silFence) != VK_SUCCESS) {
			return;
		}
	}

	// 2. Publish the region built from the readback we just consumed, then kick
	//    this frame's pair. Applying before the kick keeps the X round-trip off
	//    the critical path between submit and the next frame's wait.
	ApplyRegion(p);

	// 3. Render BOTH outermost views into the bottom `avatarFrac` of the
	//    raster — the zone-framed views map 1:1 onto that sub-viewport, so the
	//    region lands where the avatar actually is.
	const uint32_t vFirst = 0u;
	const uint32_t vLast = (p.numViews > 1) ? p.numViews - 1 : 0u;
	const bool twoViews = (p.numViews > 1);
	const uint32_t avH = (uint32_t)((float)h * p.avatarFrac + 0.5f);
	const uint32_t avatarH = (avH > 0 && avH <= h) ? avH : h;
	const uint32_t avatarY = h - avatarH;

	auto renderView = [&](uint32_t view, VkImage dst) {
		ModelRenderer::MaskProjections mp;
		mp.unrestricted = (p.projMatsUnres != nullptr) ? p.projMatsUnres[view] : nullptr;
		p.renderer->renderEye(dst, VK_FORMAT_R8G8B8A8_UNORM, w, h, 0, avatarY, w, avatarH,
		                      p.viewMats[view], p.projMats[view], /*transparentBg=*/true,
		                      (p.clipFars != nullptr) ? p.clipFars[view] : 0.0f,
		                      /*edgeFadePx=*/0.0f, &mp);
	};
	renderView(vFirst, g_silImage.image);
	if (twoViews) {
		renderView(vLast, g_silImage2.image);
	}

	// 4. Copy both planes out behind the fence — no wait. renderEye leaves the
	//    target in COLOR_ATTACHMENT_OPTIMAL.
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = g_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(p.dev, &ai, &cmd) != VK_SUCCESS) {
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
	region.bufferOffset = 0;
	vkCmdCopyImageToBuffer(cmd, g_silImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_silReadback.buffer, 1,
	                       &region);
	if (twoViews) {
		toSrc.image = g_silImage2.image;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &toSrc);
		region.bufferOffset = (VkDeviceSize)w * h * 4;
		vkCmdCopyImageToBuffer(cmd, g_silImage2.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       g_silReadback.buffer, 1, &region);
	}
	// Hand the images back in COLOR_ATTACHMENT_OPTIMAL. renderEye declares that
	// as its entry layout for any viewport whose origin is not (0,0) — which is
	// every silhouette render, since the avatar sits in the bottom band — so
	// leaving them in TRANSFER_SRC_OPTIMAL would make next frame's barrier
	// describe a layout the image is not in.
	VkImageMemoryBarrier back = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier backs[2] = {back, back};
	backs[0].image = g_silImage.image;
	backs[1].image = g_silImage2.image;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
	                     twoViews ? 2 : 1, backs);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkResetFences(p.dev, 1, &g_silFence);
	if (vkQueueSubmit(p.queue, 1, &si, g_silFence) == VK_SUCCESS) {
		g_silPending = true;
		g_silPendingTwo = twoViews;
		g_silPendingW = w;
		g_silPendingH = h;
		g_silPendingY0 = avatarY;
	}
	// `cmd` is consumed on the NEXT invocation's fence wait; freeing it here
	// while in flight would be invalid. Free the previous one instead.
	if (g_silPrevCmd != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(p.dev, g_pool, 1, &g_silPrevCmd);
	}
	g_silPrevCmd = cmd;
}

void
ClickthroughDestroy(VkDevice dev)
{
	if (g_silFence != VK_NULL_HANDLE) {
		if (g_silPending) {
			vkWaitForFences(dev, 1, &g_silFence, VK_TRUE, UINT64_MAX);
			g_silPending = false;
		}
		vkDestroyFence(dev, g_silFence, nullptr);
		g_silFence = VK_NULL_HANDLE;
	}
	if (g_silPrevCmd != VK_NULL_HANDLE && g_pool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(dev, g_pool, 1, &g_silPrevCmd);
		g_silPrevCmd = VK_NULL_HANDLE;
	}
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
	if (g_silImage2.image != VK_NULL_HANDLE) {
		modelDestroyImage(dev, g_silImage2);
	}
	if (g_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(dev, g_pool, nullptr);
		g_pool = VK_NULL_HANDLE;
	}
}
