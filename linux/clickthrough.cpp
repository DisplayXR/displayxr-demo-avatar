// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Click-through from the avatar silhouette (runtime#757).
 *
 * Ported from windows/main.cpp UpdateSilhouette + UpdateClickRegion (which use
 * SetWindowRgn); here the region goes through displayxr::linux_window's
 * set_input_region() (XShape ShapeInput on X11, the surface input region on
 * Wayland). See
 * clickthrough.h for what this port fixes relative to the first Linux cut.
 */

#include "clickthrough.h"

#include "model_renderer.h"
#include "model_vulkan_utils.h"
#include "sil_readback.h" // frame-ring-pipelined readback (INV-4.10)

#include "dxr_linux_window.h" // set_input_region / clear_input_region

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

// Scratch GPU images + per-frame-ring-slot readback bands (model_common/
// sil_readback.h), lazily created and resized with the window. TWO images: the
// coverage is the union of the outermost views and both halves must come from
// the SAME frame, or the union is stale in one eye. Single-threaded — driven
// from the frame loop.
//
// Readback pipelined by the renderer's FRAME RING (INV-4.10): the passes and
// their copies are recorded into the renderer's ONE frame command buffer; a
// band is consumed when its ring slot opens again, framesInFlight frames later
// (the renderer has waited the slot's fence by then). This replaces #837's
// private command buffer + fence — a second vkQueueSubmit per frame, waited one
// frame later — and the vkQueueWaitIdle the scratch-image init used. user[0] =
// the first raster row the avatar was drawn into.
static SilReadback g_silRb;

// Published coverage: one byte per texel, 1 = avatar present.
static std::vector<uint8_t> g_covBits;
static uint32_t g_covW = 0, g_covH = 0;
// The first row that was actually rendered. Rows above it are untouched scratch
// and must be skipped — derived from the SAME avatarY the render used, not
// recomputed from avatarFrac, because rounding the two independently can differ
// by a row and expose undefined texels.
static uint32_t g_covY0 = 0;
static bool g_covReady = false;

//! Fold the band this ring slot wrote framesInFlight frames ago into the
//! published coverage: union both planes' alpha, threshold, dilate.
static void
ConsumeLandedReadback(ModelRenderer &r, uint32_t dilate, uint8_t alphaMin)
{
	const uint8_t *px = nullptr;
	SilReadback::Meta m;
	if (!g_silRb.take(r, &px, &m)) {
		return;
	}
	const uint32_t cw = m.w, ch = m.h;
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
		if (m.planes > 1) {
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
	g_covY0 = ((uint32_t)m.user[0] < ch) ? (uint32_t)m.user[0] : 0;
	g_covReady = true;
}

//! Turn the published coverage (+ the bubble rect) into the window's input
//! region.
static void
ApplyRegion(const ClickthroughParams &p)
{
	if (!g_covReady || g_covW == 0 || g_covH == 0 || p.winW == 0 || p.winH == 0) {
		return;
	}
	std::vector<DxrWindowRect> rects;

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
				rects[i].height = (uint32_t)(bottom - (int64_t)rects[i].y);
			}
			continue;
		}
		bandFirst = rects.size();
		const int64_t top = y * winH / ch;
		for (size_t i = 0; i + 1 < runs.size(); i += 2) {
			const int64_t left = (int64_t)runs[i] * winW / cw;
			const int64_t right = (int64_t)runs[i + 1] * winW / cw;
			DxrWindowRect r;
			r.x = (int32_t)left;
			r.y = (int32_t)top;
			r.width = (uint32_t)(right > left ? right - left : 1);
			r.height = (uint32_t)(bottom > top ? bottom - top : 1);
			rects.push_back(r);
		}
		prevRuns = runs;
	}

	// The bubble sits above the avatar, outside its silhouette — union it in or
	// the shaped window makes it unclickable (and, on a shaped window, the
	// region is a VISUAL clip too).
	if (p.bubbleVisible && p.bubbleW > 0 && p.bubbleH > 0) {
		DxrWindowRect r;
		r.x = p.bubbleX;
		r.y = p.bubbleY;
		r.width = (uint32_t)p.bubbleW;
		r.height = (uint32_t)p.bubbleH;
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
	p.window->set_input_region(rects.empty() ? nullptr : rects.data(), rects.size());
}

void
ClickthroughUpdate(const ClickthroughParams &p)
{
	if (p.window == nullptr || p.winW == 0 || p.winH == 0 || p.renderer == nullptr ||
	    !p.renderer->hasModel() || p.numViews == 0 || p.viewMats == nullptr || p.projMats == nullptr) {
		return;
	}

	// Decorated (B) or opaque (Ctrl+T): the whole window is interactive. A WM
	// frame needs it for move/resize, and an opaque frame covers every pixel,
	// so shaping it to the silhouette would clip the background it just drew.
	static bool s_shapeCleared = false;
	if (p.decorated || !p.transparentBg) {
		if (!s_shapeCleared) {
			p.window->clear_input_region();
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

	// 1. Consume the band this ring slot wrote framesInFlight frames ago (the
	//    renderer waited the slot's fence when this frame opened).
	ConsumeLandedReadback(*p.renderer, s_dilate, s_alphaMin);

	static const bool s_noCached = getenv("DXR_VK_NO_HOST_CACHED") != nullptr;
	if (!g_silRb.ensure(*p.renderer, p.dev, p.phys, w, h, 2, /*preferHostCached=*/!s_noCached)) {
		return;
	}
	if (!g_silRb.hostCached && !s_noCached) {
		static bool s_warned = false;
		if (!s_warned) {
			s_warned = true;
			fprintf(stderr, "[WARN]  clickthrough: no HOST_CACHED memory type — using write-combined; "
			                "the per-frame coverage read will be slower on this driver.\n");
		}
	}
	// Fresh images → cleared COLOR_ATTACHMENT_OPTIMAL, inside the frame:
	// renderEye picks its entry layout from (viewportX == 0 && viewportY == 0),
	// and the silhouette renders at a non-zero viewportY (the avatar sits in the
	// bottom band), so it ALWAYS declares COLOR_ATTACHMENT_OPTIMAL.
	g_silRb.prepFresh(*p.renderer);

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
	renderView(vFirst, g_silRb.image(0));
	if (twoViews) {
		renderView(vLast, g_silRb.image(1));
	}

	// 4. Record both planes' copies into this frame's band — no submit, no
	//    wait. renderEye leaves the target in COLOR_ATTACHMENT_OPTIMAL; each
	//    image is handed back cleared in COLOR_ATTACHMENT_OPTIMAL, the layout
	//    renderEye declares on entry for a non-(0,0) viewport.
	const uint32_t s0 = g_silRb.record(*p.renderer, 0, 0, /*restoreCleared=*/true);
	const uint32_t s1 = twoViews ? g_silRb.record(*p.renderer, 1, 1, true) : s0;
	if (s0 == UINT32_MAX || s1 != s0) {
		return; // failed, or a target resize rolled the frame onto another slot
	}
	SilReadback::Meta m;
	m.w = w;
	m.h = h;
	m.planes = twoViews ? 2u : 1u;
	m.user[0] = (int32_t)avatarY;
	g_silRb.commit(s0, m);
}

void
ClickthroughDestroy(VkDevice dev)
{
	g_silRb.destroy(dev);
}
