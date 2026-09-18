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
 * commit c1e4fe00da0f189122eca14e61e9faaa88e4b38e. The only edit is the clearly
 * marked DEMO-LOCAL ADDITION below (a second-stage macro fallback); everything
 * else is verbatim. displayxr-common gains the same helper in v2.14.0 — once
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

#ifdef __cplusplus
}
#endif
