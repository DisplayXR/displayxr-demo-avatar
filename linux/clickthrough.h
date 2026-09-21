// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 click-through for the transparent avatar overlay (runtime#757).
 *
 * The Linux analogue of the Windows SetWindowRgn / macOS setIgnoresMouseEvents
 * silhouette click-through: render a downscaled view of the avatar into scratch
 * images, read their alpha back, and set the window's XShape INPUT region to
 * just the avatar pixels — so clicks land on the avatar and pass through the
 * transparent regions to the desktop underneath.
 *
 * Ported feature-for-feature from windows/main.cpp's UpdateSilhouette +
 * UpdateClickRegion, which the Linux version previously only approximated:
 *
 *   - the raster SCALES with the window (kTexelPxDefault window px per texel,
 *     capped), instead of a fixed 1/3 downscale, so precision does not degrade
 *     as the window grows;
 *   - the coverage is the UNION of the FIRST and LAST view, rendered in the
 *     SAME frame. The weave shows every view at once with horizontal disparity
 *     that grows with window size, so a single-view mask visibly clips the
 *     other view's avatar;
 *   - the alpha threshold is LOW (8, not 40) and the result is DILATED by one
 *     texel. The error is asymmetric: an over-large region leaves a few
 *     transparent pixels clickable, an under-large one deletes content — and
 *     on X11, as on Win32, the input shape is what makes the window reachable
 *     at all;
 *   - the readback is PIPELINED behind a fence and HOST_CACHED, instead of a
 *     synchronous vkQueueWaitIdle. That synchronous stall is the whole reason
 *     the old code ran at ~4 Hz; the region can now be rebuilt every frame and
 *     stops trailing a moving avatar;
 *   - the avatar is rendered into the BOTTOM `avatarFrac` of the raster, which
 *     is where the display-zone actually puts it. The old full-height render
 *     produced a vertically misplaced region;
 *   - the top band and the speech-bubble rect are handled explicitly: the band
 *     is skipped when scanning (nothing is drawn there) and the bubble rect is
 *     unioned in, so the bubble is not clipped away by the shaped window.
 *
 * Also note the destination-image usage fix: ModelRenderer::renderEye BLITS
 * into its target, so the scratch images need VK_IMAGE_USAGE_TRANSFER_DST_BIT.
 * The previous COLOR_ATTACHMENT|TRANSFER_SRC set was a spec violation that
 * happened to work.
 *
 * No-op when the window / Display is null (hosted-NULL fallback), and reports
 * once when the server has no XShape extension instead of silently doing
 * nothing. Requires libXext.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <cstdint>

class ModelRenderer;

//! Everything one click-through update needs. Grouped because the argument
//! list grew past the point where positional floats are readable.
struct ClickthroughParams {
	VkDevice dev = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	ModelRenderer *renderer = nullptr;

	Display *dpy = nullptr;
	Window win = 0;
	uint32_t winW = 0; //!< live client width in px
	uint32_t winH = 0; //!< live client height in px

	//! Per-view matrices for THIS frame; the first and last are rendered.
	const float (*viewMats)[16] = nullptr;
	const float (*projMats)[16] = nullptr;
	//! Unrestricted-far projections for the content-mask coverage pass
	//! (runtime#1470). nullptr = the mask falls back to the clipped alpha.
	const float (*projMatsUnres)[16] = nullptr;
	//! Per-view shader far-cull (dxr::ClipPlanes::clipFar); nullptr = no cull.
	const float *clipFars = nullptr;
	uint32_t numViews = 0;

	//! Fraction of the window height, measured from the BOTTOM, that the
	//! avatar occupies (the 3D display-zone rect). The rest is the Local2D
	//! speech-bubble band.
	float avatarFrac = 1.0f;

	//! Draw with a transparent background (Ctrl+T). An opaque frame covers the
	//! whole window, so the region must too.
	bool transparentBg = true;

	//! Decorated windows drop the shape entirely: the WM frame needs the whole
	//! window interactive for move/resize.
	bool decorated = false;

	//! Speech-bubble rect in client px, unioned into the region so the bubble
	//! stays visible and clickable outside the avatar silhouette.
	bool bubbleVisible = false;
	int32_t bubbleX = 0, bubbleY = 0, bubbleW = 0, bubbleH = 0;
};

//! Render the avatar silhouette and set the window's XShape input region from
//! it. Call once per frame, after the eye render.
void ClickthroughUpdate(const ClickthroughParams &p);

//! Free the scratch images / readback buffer / command pool / fence. Device
//! must be idle.
void ClickthroughDestroy(VkDevice dev);
