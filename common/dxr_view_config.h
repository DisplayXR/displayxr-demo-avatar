// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pick the view configuration an N-view DisplayXR app must begin with.
 *
 * Issue #1486 (option B). The runtime used to report the device's MAX view
 * count (4 on sim_display) under XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
 * which is a spec deviation: PRIMARY_STEREO means exactly 2 views. It now
 * reports EXACTLY 2 under PRIMARY_STEREO and rejects an xrEndFrame whose
 * projection layer carries viewCount > 2. The device max moved to a new
 * DisplayXR view configuration,
 * XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, which is enumerated after
 * PRIMARY_STEREO whenever the instance enabled XR_DXR_display_info.
 *
 * So ANY app whose per-frame view count comes from the active DXR rendering
 * mode (xrEnumerateDisplayRenderingModesDXR / the 1-2-3 mode keys), or that
 * sizes its zone tiles from the reported view count, MUST begin its session
 * with PRIMARY_MULTIVIEW_DXR — otherwise it goes black in Quad mode.
 * Stereo-fixed apps (hardcoded 2) stay on PRIMARY_STEREO and need none of this.
 *
 * Usage — one call, right after xrGetSystem() and before the first
 * xrEnumerateViewConfigurationViews() / xrBeginSession() / xrLocateViews():
 *
 *     xr.viewConfigType = DxrSelectViewConfigType(xr.instance, xr.systemId);
 *
 * and then feed that SAME variable to every view-configuration-typed call.
 * The helper degrades to PRIMARY_STEREO on an older runtime (or when
 * XR_DXR_display_info was not enabled on the instance), so it is safe to call
 * unconditionally.
 *
 * Header-only and C-compatible on purpose: the Windows cubes get it through
 * displayxr::common's XrSessionManager, the macOS/Linux/Android cubes carry
 * their own session code and apply it to their own variable.
 */

/*
 * VENDORED from displayxr-runtime `test_apps/common/dxr_view_config.h` at
 * commit c1e4fe00da0f189122eca14e61e9faaa88e4b38e. The edits are the clearly
 * marked DEMO-LOCAL ADDITION below (a second-stage macro fallback) and the
 * later-vendored ADR-041 DxrAliasInactiveViews at the bottom; everything else
 * is verbatim. displayxr-common gains the same helper in v2.14.0 — once
 * common/CMakeLists.txt re-pins to that tag this file can be deleted and the
 * include switched, which is a one-liner per leg.
 *
 * It lives in common/ rather than openxr_includes/ on purpose: openxr_includes/
 * is byte-pinned to the runtime by scripts/check_vendored_headers.py, and this
 * is an app-side helper, not part of the wire protocol.
 */

#pragma once

#include <stdint.h>

#include <openxr/openxr.h>

/*
 * The type value is normally supplied by this repo's
 * src/external/openxr_includes copy of XR_DXR_display_info.h (SPEC_VERSION >=
 * 19), so PREFER that header whenever it is on the include path. Probing for
 * the header rather than for the macro matters: once the enumerator is
 * registered with Khronos it becomes a real XrViewConfigurationType enumerator,
 * and a plain `#ifndef` fallback would then still fire and silently shadow it
 * with a macro of our own.
 */
#if defined(__has_include)
#if __has_include(<openxr/XR_DXR_display_info.h>)
#define DXR_VIEW_CONFIG_HAVE_DISPLAY_INFO_HEADER 1
#endif
#endif

#if defined(DXR_VIEW_CONFIG_HAVE_DISPLAY_INFO_HEADER)
#include <openxr/XR_DXR_display_info.h>
#else
/*
 * A tree that vendors an older header set (or a compiler with no __has_include)
 * still compiles — the value is fixed by the DXR author-ID block, and the
 * enumerate probe below is what actually decides whether the runtime has it.
 */
#define XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR ((XrViewConfigurationType)1004999212)
#endif

/*
 * ---- DEMO-LOCAL ADDITION (not in the runtime's copy) ------------------------
 *
 * Second-stage fallback. The block above only defines the value when the
 * display_info header is ABSENT. This repo vendors that header, so the include
 * fires — but a stale checkout (or a bisect across the openxr_includes bump in
 * this same PR) can have a PRESENT header that is still SPEC_VERSION 1 and so
 * carries no enumerator. Probing the macro here catches exactly that window and
 * cannot shadow a real Khronos enumerator, because it only runs after the
 * header that would define it has already been included.
 */
#ifndef XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR
#define XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR ((XrViewConfigurationType)1004999212)
#endif
/* ---- end demo-local addition ---------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR when the runtime
 * enumerates it for this instance+system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
 * otherwise. Never fails; any error path falls back to PRIMARY_STEREO.
 */
static inline XrViewConfigurationType
DxrSelectViewConfigType(XrInstance instance, XrSystemId systemId)
{
	XrViewConfigurationType types[8];
	uint32_t count = 0;
	uint32_t i;

	if (instance == XR_NULL_HANDLE || systemId == XR_NULL_SYSTEM_ID) {
		return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	}
	if (xrEnumerateViewConfigurations(instance, systemId, 0, &count, NULL) != XR_SUCCESS || count == 0) {
		return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	}
	if (count > 8) {
		count = 8;
	}
	if (xrEnumerateViewConfigurations(instance, systemId, count, &count, types) != XR_SUCCESS) {
		return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	}
	for (i = 0; i < count; i++) {
		if (types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) {
			return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
		}
	}
	return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
}

/*!
 * Human-readable name for a log line. Only the two types this helper can
 * return are named; anything else comes back as "other".
 */
static inline const char *
DxrViewConfigTypeName(XrViewConfigurationType t)
{
	if (t == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) {
		return "PRIMARY_MULTIVIEW_DXR";
	}
	if (t == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
		return "PRIMARY_STEREO";
	}
	return "other";
}

/* ---- ADR-041 alias helper (copied from the runtime's test_apps/common) ------
 *
 * DxrAliasInactiveViews() below is byte-for-byte the runtime's
 * test_apps/common/dxr_view_config.h helper (runtime ADR-041, 2026-09-18). It
 * is carried here, next to DxrSelectViewConfigType, because the displayxr-common
 * tag this demo pins (v2.14.0) predates it. Drop it with the rest of this file
 * when displayxr-common ships it.
 *
 * Why it is load-bearing: under PRIMARY_MULTIVIEW_DXR (which every leg opts
 * into via DxrSelectViewConfigType) xrEndFrame REFUSES a projection layer whose
 * viewCount is below xrLocateViews' count — there is no compat arm for that
 * type. A 2D mode renders one view, so without the alias every 2D frame was
 * rejected and the panel kept showing the last woven 3D frame on a lens-off
 * display: a frozen double image.
 */
/*!
 * ADR-041: fill the INACTIVE tail of a projection layer so the layer carries
 * the full located view count.
 *
 * The view count a session gets is fixed for its lifetime; what changes per
 * frame is how many of those views the active rendering mode actually uses
 * (chain XrViewActivityStateDXR on XrViewState to read it, or derive it from
 * the mode as these apps do). Core OpenXR still requires EVERY located view to
 * be supplied at xrEndFrame, so an app that renders only the active ones closes
 * the gap by pointing each inactive view at content it already rendered. The
 * runtime ignores those pixels.
 *
 * Each inactive view keeps its OWN located pose/fov — they are valid (the
 * runtime parks them at view 0's pose) and a pose the runtime rejects would
 * fail the layer for real. Only the subimage is aliased, onto view 0's.
 *
 * Safe to call with active >= located (does nothing).
 *
 * @param projViews  The layer's view array, sized @p located. [0, active) must
 *                   already be filled by the caller.
 * @param views      The XrView array xrLocateViews wrote, sized @p located.
 * @param located    What xrLocateViews returned (viewCountOutput).
 * @param active     Views the app actually rendered this frame.
 */
static inline void
DxrAliasInactiveViews(XrCompositionLayerProjectionView *projViews,
		      const XrView *views, uint32_t located, uint32_t active) {
	/*
	 * `active == 0` means the app rendered NOTHING this frame, so
	 * projViews[0] holds no subimage to alias — it is still
	 * zero-initialised, i.e. XR_NULL_HANDLE. Stamping that over every view
	 * would turn a frame with no content into a layer full of null
	 * swapchains. Reachable: a zones app whose per-zone tile count clamps to
	 * 0 (cube_zones_texture_d3d11_win). Do nothing — the caller's own "skip
	 * this layer" gate handles that frame.
	 */
	if (projViews == NULL || active == 0 || active >= located) {
		return;
	}
	for (uint32_t i = active; i < located; i++) {
		projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		projViews[i].next = NULL;
		if (views != NULL) {
			projViews[i].pose = views[i].pose;
			projViews[i].fov = views[i].fov;
		} else {
			projViews[i].pose = projViews[0].pose;
			projViews[i].fov = projViews[0].fov;
		}
		// The whole trick: content the app DID render this frame, which
		// the runtime then discards because the view is inactive.
		projViews[i].subImage = projViews[0].subImage;
	}
}

#ifdef __cplusplus
}
#endif
