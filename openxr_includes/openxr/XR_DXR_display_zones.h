// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not. SPEC_VERSION restarted at 1 on the XR_EXT_* -> XR_DXR_* rename.
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_display_zones extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * Lets an app compose, within its window, N 3D zones (each a window-pixel
 * rect with its own view-rig framing and its own multiview projection layer),
 * M 2D zones, and one wish mask (window-space, any-resolution, per-pixel
 * M in [0,1]) telling the hardware which regions to physically switch to 3D.
 *
 * It unifies three existing mechanisms BY COMPOSITION — it requires and
 * reuses, rather than replaces:
 *
 *  - 3D framing per zone = the XR_DXR_view_rig descriptors (XrDisplayRigDXR /
 *    XrCameraRigDXR), chained per-locate exactly as today; the new
 *    XrDisplayZoneDXR on the same locate scopes the framing to the zone's
 *    rect (the rect IS the canvas).
 *  - 2D zones = XrCompositionLayerLocal2DDXR from XR_DXR_local_3d_zone,
 *    verbatim.
 *  - Explicit wish mask = XrLocal3DZoneMaskDXR and its three authoring tiers,
 *    verbatim — referenced per-frame from the xrEndFrame chain instead of the
 *    sticky xrSubmitLocal3DZoneDXR channel.
 *
 * A frame is a ZONES FRAME iff >= 1 projection layer carries an
 * XrDisplayZoneDXR chain. In a zones frame every projection layer must carry
 * one (all-or-none, else XR_ERROR_VALIDATION_FAILURE); the former output-rect
 * path, the sticky xrSubmitLocal3DZoneDXR mask, and the implicit-mask-from-
 * Local2D rule are all inert (ignored, not an error). Zero-zone frames behave
 * per XR_DXR_local_3d_zone v3 verbatim —
 * back-compat is structural, nothing is deprecated.
 *
 * Requires XR_DXR_local_3d_zone (>= v4) + XR_DXR_view_rig (>= v2).
 * Extension-app classes only. Full design: docs/adr/ADR-027-display-zones.md;
 * spec: docs/specs/extensions/XR_DXR_display_zones.md.
 */
#ifndef XR_DXR_DISPLAY_ZONES_H
#define XR_DXR_DISPLAY_ZONES_H 1

#include <openxr/openxr.h>
#include <openxr/XR_DXR_local_3d_zone.h> // XrLocal3DZoneMaskDXR — reused, never re-declared
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_display_zones 1
#define XR_DXR_display_zones_SPEC_VERSION 1
#define XR_DXR_DISPLAY_ZONES_EXTENSION_NAME "XR_DXR_display_zones"

// Extension type-value range (1004999xxx); replace with a Khronos-assigned
// value if standardized. Allocation registry: README.md in this directory.
#define XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR               ((XrStructureType)1004999150)
#define XR_TYPE_DISPLAY_ZONE_DXR                            ((XrStructureType)1004999151)
#define XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR            ((XrStructureType)1004999152)
#define XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR ((XrStructureType)1004999153)

typedef XrFlags64 XrDisplayZonesFrameEndFlagsDXR;
//! Cross-check zone/locate/mask consistency this frame: zone rects vs wish
//! coverage, locate-rect vs submit-rect, zoneId pairing. One-shot WARN per
//! violation class per session — never a per-frame error.
#define XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR ((XrDisplayZonesFrameEndFlagsDXR)0x00000001)

/*!
 * @brief Capabilities of the display-zones path for a session.
 *
 * @c maxZones3D is plugin-independent (zone assembly is compositor-side; see
 * ADR-027 Decision 5). Hardware switching granularity is deliberately NOT
 * exposed here — the wish is advisory; the only advisory hardware hint
 * remains hardwareZoneGridWidth/Height in XrLocal3DZoneCapabilitiesDXR.
 */
typedef struct XrDisplayZoneCapabilitiesDXR {
    XrStructureType    type;       //!< XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR
    void* XR_MAY_ALIAS next;
    XrBool32           supported;  //!< XR_FALSE => only the legacy single-canvas path
    uint32_t           maxZones3D; //!< max zone-chained projection layers per frame
} XrDisplayZoneCapabilitiesDXR;

/*!
 * @brief A 3D display zone: identity + placement. Valid at TWO chain points:
 *
 *  - XrViewLocateInfo::next — a ZONE-SCOPED LOCATE: the runtime computes the
 *    Kooima projection framed to @c rect (the rect IS the canvas) and returns
 *    render-ready XrView{pose, fov} for this zone. Chain a rig descriptor
 *    (XrDisplayRigDXR / XrCameraRigDXR) on the same locate as usual; an
 *    XrViewDisplayRawDXR chained on the result reports canvasRectPx /
 *    canvasSizeMeters for THE ZONE.
 *
 *  - XrCompositionLayerProjection::next — binds that layer's views to the
 *    zone at xrEndFrame.
 *
 * Chain the SAME instance at both points within a frame. The xrEndFrame
 * values are authoritative; a locate/submit rect divergence mis-frames one
 * frame (same latency class as pose-prediction error — and the same
 * consistency burden core OpenXR already places on apps when copying XrView
 * pose/fov into XrCompositionLayerProjectionView). VALIDATE_BIT warns.
 *
 * @c rect is in client-window pixels — the same space as
 * XrCompositionLayerLocal2DDXR::rect and the mask authoring tiers.
 *
 * @c zoneId exists for locate<->submit pairing (validate mode), debug
 * logs/captures, and as the stable referent for the reserved effective-mask
 * readback (spec v2). There is NO zone handle — zones are stateless
 * per-frame data, like layers; only the mask owns GPU resources and already
 * has a handle.
 */
typedef struct XrDisplayZoneDXR {
    XrStructureType          type;   //!< XR_TYPE_DISPLAY_ZONE_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 zoneId; //!< app-chosen; unique among this frame's 3D zones
    XrRect2Di                rect;   //!< client-window pixels
} XrDisplayZoneDXR;

/*!
 * @brief Per-frame wish reference. Optional, chained on XrFrameEndInfo::next
 *        in a zones frame.
 *
 * Absent, or wishMask == XR_NULL_HANDLE: the wish AUTO-DERIVES as the union
 * of the frame's 3D-zone rects with an implementation-defined feather.
 *
 * Present with a mask: that mask is the frame's wish verbatim — atomic with
 * the layer set. In zones mode the wish is HARDWARE-ONLY: it does not gate
 * compositor blending (composition follows zone geometry + alpha).
 *
 * Wish semantics: per-pixel M in [0,1] — 1.0 = panel physically 3D, 0.0 =
 * flat, intermediate = fractional 3D-ness at the display processor's
 * discretion (blend, dither, threshold). The wish is ADVISORY: the DP may
 * quantize, dilate, or snap it to its switching-cell granularity, coalesce
 * updates, or ignore components the firmware cannot express. The app MUST
 * NOT assume the physical state equals the wish.
 */
typedef struct XrDisplayZonesFrameEndInfoDXR {
    XrStructureType                type;     //!< XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR
    const void* XR_MAY_ALIAS       next;
    XrDisplayZonesFrameEndFlagsDXR flags;
    XrLocal3DZoneMaskDXR           wishMask; //!< XR_NULL_HANDLE = auto-derive
} XrDisplayZonesFrameEndInfoDXR;

/*!
 * @brief Advisory: per-zone recommended view sizes may have changed
 *        (display-mode / tile-count switch, window DPI change).
 *
 * Re-query each zone via xrGetDisplayZoneRecommendedViewSizeDXR; stale sizes
 * stay correct, just soft (the runtime scaled-blits view tiles to rects). The
 * N-zone analog of XrEventDataLocal3DZoneViewSizeChangedDXR (which is
 * single-size and cannot describe N zones; it keeps firing for legacy
 * sessions).
 */
typedef struct XrEventDataDisplayZoneMetricsChangedDXR {
    XrStructureType          type;    //!< XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
} XrEventDataDisplayZoneMetricsChangedDXR;

typedef XrResult (XRAPI_PTR *PFN_xrGetDisplayZoneCapabilitiesDXR)(
    XrSession session, XrDisplayZoneCapabilitiesDXR* capabilities);

typedef XrResult (XRAPI_PTR *PFN_xrGetDisplayZoneRecommendedViewSizeDXR)(
    XrSession session, const XrRect2Di* zoneRect, XrExtent2Di* recommendedViewSize);

#ifndef XR_NO_PROTOTYPES

//! Query whether the session can consume zone-chained projection layers.
XRAPI_ATTR XrResult XRAPI_CALL xrGetDisplayZoneCapabilitiesDXR(
    XrSession session, XrDisplayZoneCapabilitiesDXR* capabilities);

//! Pure query: recommended per-view image size for a zone of this rect under
//! the current display mode — the zone-rect analog of
//! xrEnumerateViewConfigurationViews. Zones share the session's view COUNT
//! (display modes are session-global); only per-view dimensions vary by rect.
XRAPI_ATTR XrResult XRAPI_CALL xrGetDisplayZoneRecommendedViewSizeDXR(
    XrSession session, const XrRect2Di* zoneRect, XrExtent2Di* recommendedViewSize);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_DXR_DISPLAY_ZONES_H */
