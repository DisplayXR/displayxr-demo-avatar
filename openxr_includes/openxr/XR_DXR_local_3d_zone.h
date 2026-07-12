// Copyright 2026, The DisplayXR Project
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
 * @brief  Header for XR_DXR_local_3d_zone extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * Lets an application declare which regions of its window are 3D vs 2D via a
 * per-pixel scalar "3D-ness" mask, authored at one of three tiers (whole
 * window / rect list / freeform render target). The single authored mask has
 * up to two runtime-side consumers that must agree (see
 * docs/roadmap/unified-2d-3d-compositing.md §2):
 *
 *   - the COMPOSITOR (this leg): software-composites the flat 2D layer over
 *     the weaved 3D output, gated by the mask (mask-lerp, full resolution);
 *   - the HARDWARE DP (separate leg, docs/roadmap/local-3d-zones.md):
 *     publishes the mask to the switchable-lens panel so the cells over a 3D
 *     region are in 3D mode and the rest are flat.
 *
 * The mask is a SEPARATE scalar channel, NOT the 2D layer's alpha — region
 * selection and content transparency are independent (spec §4.0). M = 1 → 3D
 * (keep the weave), M = 0 → 2D, fractional only at anti-aliased boundaries.
 *
 * Spec v3 (#439 Phase 3) adds the 2D side of the story as a first-class
 * composition layer: XrCompositionLayerLocal2DDXR, a post-weave screen-space
 * 2D layer submitted through the normal xrEndFrame layer list (replacing the
 * shared-texture surround side-channel as the 2D source), plus
 * XrEventDataLocal3DZoneViewSizeChangedDXR, the view-size renegotiation
 * event. Design: docs/roadmap/unified-2d-3d-phase3-impl.md.
 */
#ifndef XR_DXR_LOCAL_3D_ZONE_H
#define XR_DXR_LOCAL_3D_ZONE_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_local_3d_zone 1
// SPEC_VERSION 4: XrStructureType values relocated 1004999130..136 ->
// 1004999160..166 (the 130..132 block collided with XR_DXR_mcp_tools, which
// reserved it first). No struct/field/entry-point changes; consumers only
// need a header re-sync + rebuild. See README.md (allocation registry) in
// this directory.
#define XR_DXR_local_3d_zone_SPEC_VERSION 1
#define XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME "XR_DXR_local_3d_zone"

// Extension type-value range (1004999xxx); replace with a Khronos-assigned
// value if standardized. Allocation registry: README.md in this directory.
#define XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_DXR      ((XrStructureType)1004999160)
#define XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR  ((XrStructureType)1004999161)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR ((XrStructureType)1004999162)
// Spec v2 additions — D3D12 + Vulkan Tier-3 bindings. (No Metal Tier-3
// binding yet — Tier 1/2 are API-agnostic and fully supported on Metal;
// xrAcquireLocal3DZoneRenderTargetDXR reports XR_ERROR_FEATURE_UNSUPPORTED.)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR  ((XrStructureType)1004999163)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_DXR ((XrStructureType)1004999164)
// Spec v3 additions — the post-weave 2D composition layer + the view-size
// renegotiation event (#439 Phase 3).
#define XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR                  ((XrStructureType)1004999165)
#define XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_DXR ((XrStructureType)1004999166)

XR_DEFINE_HANDLE(XrLocal3DZoneMaskDXR)

/*!
 * @brief Capabilities of the local-3D-zone path for a session.
 *
 * @c supported is true when the runtime can consume an authored mask. In this
 * runtime the COMPOSITOR consumer is always available on a 3D display; the
 * @c hardwareZoneGridWidth/Height fields describe the switchable-lens grid of
 * the active display processor (1×1 = global on/off; 0 = no hardware-zone DP
 * yet, compositor-only). @c maxMaskWidth/Height bound the authored mask size.
 */
typedef struct XrLocal3DZoneCapabilitiesDXR {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_DXR
    void* XR_MAY_ALIAS next;
    XrBool32           supported;
    uint32_t           hardwareZoneGridWidth;
    uint32_t           hardwareZoneGridHeight;
    uint32_t           maxMaskWidth;
    uint32_t           maxMaskHeight;
} XrLocal3DZoneCapabilitiesDXR;

/*!
 * @brief Parameters for creating a mask, bound to the session's window.
 *
 * @c maskWidth/Height is the authored mask resolution in client-window
 * pixels; 0 lets the runtime choose (clamped to maxMaskWidth/Height). The
 * compositor samples the mask at this resolution for crisp 2D/3D edges
 * (spec §9 Q1); the hardware-DP leg, when present, downsamples it.
 */
typedef struct XrLocal3DZoneMaskCreateInfoDXR {
    XrStructureType          type;   //!< XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 maskWidth;
    uint32_t                 maskHeight;
} XrLocal3DZoneMaskCreateInfoDXR;

/*!
 * @brief Tier-3 freeform binding: the D3D11 render target the app draws the
 *        mask into. Returned (in the next chain) by
 *        xrAcquireLocal3DZoneRenderTargetDXR. The RTV is on an R8_UNORM
 *        texture in client-window pixels; write M (3D-ness) to .r.
 */
typedef struct XrLocal3DZoneRenderTargetD3D11DXR {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR
    void* XR_MAY_ALIAS next;
    void*              renderTargetView; //!< ID3D11RenderTargetView*
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetD3D11DXR;

/*!
 * @brief Tier-3 freeform binding, D3D12 variant (spec v2). The runtime
 *        returns the ID3D12Resource* (R8_UNORM, client-window pixels); the
 *        app creates its OWN render-target descriptor on it (descriptor
 *        heaps are app-owned in D3D12) and writes M (3D-ness) to .r.
 *
 * Sync contract: the in-process D3D12 native compositor runs on the app's
 * device AND command queue, so same-queue submission order is the contract —
 * execute the mask draw, then call xrSubmitLocal3DZoneDXR. No fence.
 *
 * State contract: the resource is handed out in
 * D3D12_RESOURCE_STATE_RENDER_TARGET; the app must transition it back to
 * RENDER_TARGET before calling xrSubmitLocal3DZoneDXR (the runtime's Tier-1/2
 * clears and submit snapshot assume that steady state).
 */
typedef struct XrLocal3DZoneRenderTargetD3D12DXR {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR
    void* XR_MAY_ALIAS next;
    void*              resource; //!< ID3D12Resource*
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetD3D12DXR;

/*!
 * @brief Tier-3 freeform binding, Vulkan variant (spec v2). VkImage +
 *        VkImageView on an R8_UNORM image in client-window pixels; write M
 *        (3D-ness) to .r. The vk_native compositor shares the app's
 *        VkDevice, so the handles are directly usable by the app; same-queue
 *        submission ordering is the sync contract (as D3D12).
 */
typedef struct XrLocal3DZoneRenderTargetVulkanDXR {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_DXR
    void* XR_MAY_ALIAS next;
    void*              image;     //!< VkImage
    void*              imageView; //!< VkImageView
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetVulkanDXR;

/*!
 * @brief Post-weave 2D content at a client-window pixel rect, mask-gated
 *        (spec v3, #439 Phase 3).
 *
 * Submitted through the normal xrEndFrame layer list beside the projection
 * layers; composited POST-weave at full resolution:
 *
 *     final = M·weave + (1−M)·flatten(local2D layers)
 *
 * @c rect places @c subImage at a client-window pixel rect (post-DPI; the
 * same coordinate space as the mask tiers — spec §5.1). Outside the rect the
 * layer contributes transparent 2D; where M = 0 and no 2D coverage,
 * final.a → 0 and the compose-under-bg path shows the desktop. A full-window
 * rect is the degenerate case (reproduces the legacy surround source).
 *
 * Multiple Local2D layers flatten in layer-list order (later = on top) with
 * premultiplied-alpha "over"; XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
 * is honored per layer. Local2D layers SUPERSEDE a registered surround
 * texture for that frame (newer authority wins totally).
 *
 * It is valid to submit Local2D layers without ever creating a mask object:
 * with no active mask, the union of the frame's Local2D layer rects implies
 * an IMPLICIT mask (M = 0 inside the rects, M = 1 elsewhere) that behaves
 * exactly like an explicit Tier-2 mask built from those rects — including
 * superseding the canvas output rect (uniform rule: any active mask
 * supersedes; no third state). An explicit submitted mask takes total
 * authority. The extension must still be enabled on the instance.
 */
typedef struct XrCompositionLayerLocal2DDXR {
    XrStructureType             type;       //!< XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR
    const void* XR_MAY_ALIAS    next;
    XrCompositionLayerFlags     layerFlags; //!< alpha bits honored
    XrSwapchainSubImage         subImage;   //!< source texture + sub-rect
    XrRect2Di                   rect;       //!< client-window pixels, dest
} XrCompositionLayerLocal2DDXR;

/*!
 * @brief Queued when the runtime's recommended view size changes — mask
 *        activation / deactivation / window resize (spec v3, #439 Phase 3).
 *
 * The app should recreate its projection swapchains at the new size. Purely
 * advisory — the projection pass scales arbitrary submitted sizes, so a
 * laggy app stays correct (just soft); there is no hard protocol step.
 * Fired only when the dimensions actually change.
 */
typedef struct XrEventDataLocal3DZoneViewSizeChangedDXR {
    XrStructureType          type;   //!< XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session; //!< The session whose view size changed
    uint32_t                 recommendedImageRectWidth;
    uint32_t                 recommendedImageRectHeight;
} XrEventDataLocal3DZoneViewSizeChangedDXR;

typedef XrResult (XRAPI_PTR *PFN_xrGetLocal3DZoneCapabilitiesDXR)(
    XrSession session, XrLocal3DZoneCapabilitiesDXR* capabilities);

typedef XrResult (XRAPI_PTR *PFN_xrCreateLocal3DZoneMaskDXR)(
    XrSession session, const XrLocal3DZoneMaskCreateInfoDXR* createInfo, XrLocal3DZoneMaskDXR* mask);

typedef XrResult (XRAPI_PTR *PFN_xrSetLocal3DZoneWholeWindowDXR)(
    XrLocal3DZoneMaskDXR mask, XrBool32 enable3D);

typedef XrResult (XRAPI_PTR *PFN_xrSetLocal3DZoneFromRectsDXR)(
    XrLocal3DZoneMaskDXR mask, uint32_t rectCount, const XrRect2Di* rects);

typedef XrResult (XRAPI_PTR *PFN_xrAcquireLocal3DZoneRenderTargetDXR)(
    XrLocal3DZoneMaskDXR mask, void* binding);

typedef XrResult (XRAPI_PTR *PFN_xrSubmitLocal3DZoneDXR)(
    XrLocal3DZoneMaskDXR mask);

typedef XrResult (XRAPI_PTR *PFN_xrDestroyLocal3DZoneMaskDXR)(
    XrLocal3DZoneMaskDXR mask);

#ifndef XR_NO_PROTOTYPES

//! Query whether the session can consume an authored 2D/3D-zone mask.
XRAPI_ATTR XrResult XRAPI_CALL xrGetLocal3DZoneCapabilitiesDXR(
    XrSession session, XrLocal3DZoneCapabilitiesDXR* capabilities);

//! Create a mask bound to the session's window (HWND from a window binding).
XRAPI_ATTR XrResult XRAPI_CALL xrCreateLocal3DZoneMaskDXR(
    XrSession session, const XrLocal3DZoneMaskCreateInfoDXR* createInfo, XrLocal3DZoneMaskDXR* mask);

//! Tier 1 — whole window 3D (XR_TRUE) or 2D (XR_FALSE).
XRAPI_ATTR XrResult XRAPI_CALL xrSetLocal3DZoneWholeWindowDXR(
    XrLocal3DZoneMaskDXR mask, XrBool32 enable3D);

//! Tier 2 — the runtime rasterizes these client-window-pixel rects as the 3D
//! region (M=1 inside, M=0 elsewhere). A single rect reproduces the canvas
//! sub-rect behavior of the pre-mask surround path.
XRAPI_ATTR XrResult XRAPI_CALL xrSetLocal3DZoneFromRectsDXR(
    XrLocal3DZoneMaskDXR mask, uint32_t rectCount, const XrRect2Di* rects);

//! Tier 3 — acquire the API-typed render target to draw a freeform mask into.
//! @p binding points to an API-specific struct in the next chain (e.g.
//! XrLocal3DZoneRenderTargetD3D11DXR) which the runtime fills.
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireLocal3DZoneRenderTargetDXR(
    XrLocal3DZoneMaskDXR mask, void* binding);

//! Stage the mask's current state for the next frame submission. The mask,
//! the 2D layer, and the 3D layers are consumed as one coherent xrEndFrame
//! set (atomic per frame — spec §9 Q3). The mask stays active across frames
//! (sticky, last-submit-wins) until re-submit or destroy.
//!
//! While a submitted mask is active, the canvas output rect is superseded by
//! display-zones, not an error: the weave region, view dimensions, and
//! projection metrics span the full client window, and the mask is the sole
//! 2D/3D region selector over that window-spanning 3D scene. Destroying the
//! mask restores the default (full-window) behavior on the next frame.
//! (#439 Phase 2.)
XRAPI_ATTR XrResult XRAPI_CALL xrSubmitLocal3DZoneDXR(
    XrLocal3DZoneMaskDXR mask);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyLocal3DZoneMaskDXR(
    XrLocal3DZoneMaskDXR mask);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_DXR_LOCAL_3D_ZONE_H */
