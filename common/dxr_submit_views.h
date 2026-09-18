// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  How many projection views this frame's xrEndFrame may carry.
 *
 * INV-3.1 says: submit the ACTIVE rendering mode's view count. Runtime #1486 /
 * #1500 made that a hard gate — a projection layer whose viewCount the session's
 * view configuration does not allow now fails xrEndFrame with
 * XR_ERROR_VALIDATION_FAILURE, and because a rejected frame is never ended,
 * every later xrBeginFrame returns XR_FRAME_DISCARDED: one bad count wedges the
 * app black rather than dropping a frame.
 *
 * The active mode's count is necessary but NOT sufficient, because three
 * independent numbers have to agree and any of them can move under the app:
 *
 *   1. the ACTIVE rendering mode's view count — what the runtime wants;
 *   2. the number of views xrLocateViews actually returned — an app cannot
 *      submit a view it has no pose/fov for, and a locate can fail or come back
 *      short (mode transition in flight, session not yet ready);
 *   3. how many tiles the swapchain can hold — every leg of this demo renders
 *      into ONE worst-case-sized atlas swapchain (arraySize 1) subdivided into
 *      `tileColumns x tileRows` sub-rects, so the tile grid is this app's
 *      analogue of a layered swapchain's array slices. Writing an (N+1)th view
 *      into an N-tile grid overlaps a tile that another view already owns.
 *
 * Submitting min() of the three is always representable. Taking any one of them
 * on its own is the bug class this helper exists to close.
 *
 * Header-only and C-compatible so all four legs (windows/, macos/, linux/,
 * android/) can share it verbatim; the one-shot log is left to the caller
 * because each leg has its own logging macro.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Clamp the projection-layer view count to what is actually submittable.
 *
 * @param activeModeViews   Views in the currently active DXR rendering mode.
 *                          Pass the app's fixed count when the leg does not
 *                          enumerate rendering modes.
 * @param locatedViews      `viewCountOutput` from this frame's xrLocateViews.
 *                          Pass 0 when the locate failed — the result is then 0
 *                          and the caller must submit NO projection layer.
 * @param swapchainSlices   Tiles the atlas holds this frame (tileColumns *
 *                          tileRows), or the array size for a layered
 *                          swapchain. 0 means "unknown", and the term is
 *                          ignored.
 * @param disagreed         Optional out-flag, set to 1 when the inputs were not
 *                          already equal (i.e. something was actually clamped)
 *                          so the caller can log ONCE. Never set to 0 by this
 *                          function — initialise it yourself.
 *
 * @return min(activeModeViews, locatedViews, swapchainSlices), or 0 when
 *         nothing can be submitted. NEVER silently returns a count the caller
 *         has not rendered.
 */
static inline uint32_t
DxrClampSubmitViewCount(uint32_t activeModeViews,
                        uint32_t locatedViews,
                        uint32_t swapchainSlices,
                        int *disagreed)
{
	uint32_t n = activeModeViews;

	/* A locate that returned nothing means there is nothing to submit. Do not
	 * fall back to the mode count: that is the "app submits poses it never
	 * located" bug, and it looks like a correct frame right up to xrEndFrame. */
	if (locatedViews == 0) {
		if (disagreed != NULL && activeModeViews != 0) {
			*disagreed = 1;
		}
		return 0;
	}

	if (locatedViews < n) {
		n = locatedViews;
	}
	if (swapchainSlices > 0 && swapchainSlices < n) {
		n = swapchainSlices;
	}

	if (disagreed != NULL && n != activeModeViews) {
		*disagreed = 1;
	}
	return n;
}

#ifdef __cplusplus
}
#endif
