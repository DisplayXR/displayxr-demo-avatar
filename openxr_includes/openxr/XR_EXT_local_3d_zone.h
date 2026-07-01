// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_local_3d_zone extension
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
 * composition layer: XrCompositionLayerLocal2DEXT, a post-weave screen-space
 * 2D layer submitted through the normal xrEndFrame layer list (replacing the
 * shared-texture surround side-channel as the 2D source), plus
 * XrEventDataLocal3DZoneViewSizeChangedEXT, the view-size renegotiation
 * event. Design: docs/roadmap/unified-2d-3d-phase3-impl.md.
 */
#ifndef XR_EXT_LOCAL_3D_ZONE_H
#define XR_EXT_LOCAL_3D_ZONE_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_local_3d_zone 1
// SPEC_VERSION 4: XrStructureType values relocated 1000999130..136 ->
// 1000999160..166 (the 130..132 block collided with XR_EXT_mcp_tools, which
// reserved it first). No struct/field/entry-point changes; consumers only
// need a header re-sync + rebuild. See README.md (allocation registry) in
// this directory.
#define XR_EXT_local_3d_zone_SPEC_VERSION 4
#define XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME "XR_EXT_local_3d_zone"

// Extension type-value range (1000999xxx); replace with a Khronos-assigned
// value if standardized. Allocation registry: README.md in this directory.
#define XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_EXT      ((XrStructureType)1000999160)
#define XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT  ((XrStructureType)1000999161)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_EXT ((XrStructureType)1000999162)
// Spec v2 additions — D3D12 + Vulkan Tier-3 bindings. (No Metal Tier-3
// binding yet — Tier 1/2 are API-agnostic and fully supported on Metal;
// xrAcquireLocal3DZoneRenderTargetEXT reports XR_ERROR_FEATURE_UNSUPPORTED.)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_EXT  ((XrStructureType)1000999163)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_EXT ((XrStructureType)1000999164)
// Spec v3 additions — the post-weave 2D composition layer + the view-size
// renegotiation event (#439 Phase 3).
#define XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT                  ((XrStructureType)1000999165)
#define XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_EXT ((XrStructureType)1000999166)

XR_DEFINE_HANDLE(XrLocal3DZoneMaskEXT)

/*!
 * @brief Capabilities of the local-3D-zone path for a session.
 *
 * @c supported is true when the runtime can consume an authored mask. In this
 * runtime the COMPOSITOR consumer is always available on a 3D display; the
 * @c hardwareZoneGridWidth/Height fields describe the switchable-lens grid of
 * the active display processor (1×1 = global on/off; 0 = no hardware-zone DP
 * yet, compositor-only). @c maxMaskWidth/Height bound the authored mask size.
 */
typedef struct XrLocal3DZoneCapabilitiesEXT {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_EXT
    void* XR_MAY_ALIAS next;
    XrBool32           supported;
    uint32_t           hardwareZoneGridWidth;
    uint32_t           hardwareZoneGridHeight;
    uint32_t           maxMaskWidth;
    uint32_t           maxMaskHeight;
} XrLocal3DZoneCapabilitiesEXT;

/*!
 * @brief Parameters for creating a mask, bound to the session's window.
 *
 * @c maskWidth/Height is the authored mask resolution in client-window
 * pixels; 0 lets the runtime choose (clamped to maxMaskWidth/Height). The
 * compositor samples the mask at this resolution for crisp 2D/3D edges
 * (spec §9 Q1); the hardware-DP leg, when present, downsamples it.
 */
typedef struct XrLocal3DZoneMaskCreateInfoEXT {
    XrStructureType          type;   //!< XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS next;
    uint32_t                 maskWidth;
    uint32_t                 maskHeight;
} XrLocal3DZoneMaskCreateInfoEXT;

/*!
 * @brief Tier-3 freeform binding: the D3D11 render target the app draws the
 *        mask into. Returned (in the next chain) by
 *        xrAcquireLocal3DZoneRenderTargetEXT. The RTV is on an R8_UNORM
 *        texture in client-window pixels; write M (3D-ness) to .r.
 */
typedef struct XrLocal3DZoneRenderTargetD3D11EXT {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_EXT
    void* XR_MAY_ALIAS next;
    void*              renderTargetView; //!< ID3D11RenderTargetView*
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetD3D11EXT;

/*!
 * @brief Tier-3 freeform binding, D3D12 variant (spec v2). The runtime
 *        returns the ID3D12Resource* (R8_UNORM, client-window pixels); the
 *        app creates its OWN render-target descriptor on it (descriptor
 *        heaps are app-owned in D3D12) and writes M (3D-ness) to .r.
 *
 * Sync contract: the in-process D3D12 native compositor runs on the app's
 * device AND command queue, so same-queue submission order is the contract —
 * execute the mask draw, then call xrSubmitLocal3DZoneEXT. No fence.
 *
 * State contract: the resource is handed out in
 * D3D12_RESOURCE_STATE_RENDER_TARGET; the app must transition it back to
 * RENDER_TARGET before calling xrSubmitLocal3DZoneEXT (the runtime's Tier-1/2
 * clears and submit snapshot assume that steady state).
 */
typedef struct XrLocal3DZoneRenderTargetD3D12EXT {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_EXT
    void* XR_MAY_ALIAS next;
    void*              resource; //!< ID3D12Resource*
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetD3D12EXT;

/*!
 * @brief Tier-3 freeform binding, Vulkan variant (spec v2). VkImage +
 *        VkImageView on an R8_UNORM image in client-window pixels; write M
 *        (3D-ness) to .r. The vk_native compositor shares the app's
 *        VkDevice, so the handles are directly usable by the app; same-queue
 *        submission ordering is the sync contract (as D3D12).
 */
typedef struct XrLocal3DZoneRenderTargetVulkanEXT {
    XrStructureType    type;   //!< XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_EXT
    void* XR_MAY_ALIAS next;
    void*              image;     //!< VkImage
    void*              imageView; //!< VkImageView
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetVulkanEXT;

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
typedef struct XrCompositionLayerLocal2DEXT {
    XrStructureType             type;       //!< XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT
    const void* XR_MAY_ALIAS    next;
    XrCompositionLayerFlags     layerFlags; //!< alpha bits honored
    XrSwapchainSubImage         subImage;   //!< source texture + sub-rect
    XrRect2Di                   rect;       //!< client-window pixels, dest
} XrCompositionLayerLocal2DEXT;

/*!
 * @brief Queued when the runtime's recommended view size changes — mask
 *        activation / deactivation / window resize (spec v3, #439 Phase 3).
 *
 * The app should recreate its projection swapchains at the new size. Purely
 * advisory — the projection pass scales arbitrary submitted sizes, so a
 * laggy app stays correct (just soft); there is no hard protocol step.
 * Fired only when the dimensions actually change.
 */
typedef struct XrEventDataLocal3DZoneViewSizeChangedEXT {
    XrStructureType          type;   //!< XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_EXT
    const void* XR_MAY_ALIAS next;
    XrSession                session; //!< The session whose view size changed
    uint32_t                 recommendedImageRectWidth;
    uint32_t                 recommendedImageRectHeight;
} XrEventDataLocal3DZoneViewSizeChangedEXT;

typedef XrResult (XRAPI_PTR *PFN_xrGetLocal3DZoneCapabilitiesEXT)(
    XrSession session, XrLocal3DZoneCapabilitiesEXT* capabilities);

typedef XrResult (XRAPI_PTR *PFN_xrCreateLocal3DZoneMaskEXT)(
    XrSession session, const XrLocal3DZoneMaskCreateInfoEXT* createInfo, XrLocal3DZoneMaskEXT* mask);

typedef XrResult (XRAPI_PTR *PFN_xrSetLocal3DZoneWholeWindowEXT)(
    XrLocal3DZoneMaskEXT mask, XrBool32 enable3D);

typedef XrResult (XRAPI_PTR *PFN_xrSetLocal3DZoneFromRectsEXT)(
    XrLocal3DZoneMaskEXT mask, uint32_t rectCount, const XrRect2Di* rects);

typedef XrResult (XRAPI_PTR *PFN_xrAcquireLocal3DZoneRenderTargetEXT)(
    XrLocal3DZoneMaskEXT mask, void* binding);

typedef XrResult (XRAPI_PTR *PFN_xrSubmitLocal3DZoneEXT)(
    XrLocal3DZoneMaskEXT mask);

typedef XrResult (XRAPI_PTR *PFN_xrDestroyLocal3DZoneMaskEXT)(
    XrLocal3DZoneMaskEXT mask);

#ifndef XR_NO_PROTOTYPES

//! Query whether the session can consume an authored 2D/3D-zone mask.
XRAPI_ATTR XrResult XRAPI_CALL xrGetLocal3DZoneCapabilitiesEXT(
    XrSession session, XrLocal3DZoneCapabilitiesEXT* capabilities);

//! Create a mask bound to the session's window (HWND from a window binding).
XRAPI_ATTR XrResult XRAPI_CALL xrCreateLocal3DZoneMaskEXT(
    XrSession session, const XrLocal3DZoneMaskCreateInfoEXT* createInfo, XrLocal3DZoneMaskEXT* mask);

//! Tier 1 — whole window 3D (XR_TRUE) or 2D (XR_FALSE).
XRAPI_ATTR XrResult XRAPI_CALL xrSetLocal3DZoneWholeWindowEXT(
    XrLocal3DZoneMaskEXT mask, XrBool32 enable3D);

//! Tier 2 — the runtime rasterizes these client-window-pixel rects as the 3D
//! region (M=1 inside, M=0 elsewhere). A single rect reproduces the canvas
//! sub-rect behavior of the pre-mask surround path.
XRAPI_ATTR XrResult XRAPI_CALL xrSetLocal3DZoneFromRectsEXT(
    XrLocal3DZoneMaskEXT mask, uint32_t rectCount, const XrRect2Di* rects);

//! Tier 3 — acquire the API-typed render target to draw a freeform mask into.
//! @p binding points to an API-specific struct in the next chain (e.g.
//! XrLocal3DZoneRenderTargetD3D11EXT) which the runtime fills.
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireLocal3DZoneRenderTargetEXT(
    XrLocal3DZoneMaskEXT mask, void* binding);

//! Stage the mask's current state for the next frame submission. The mask,
//! the 2D layer, and the 3D layers are consumed as one coherent xrEndFrame
//! set (atomic per frame — spec §9 Q3). The mask stays active across frames
//! (sticky, last-submit-wins) until re-submit or destroy.
//!
//! While a submitted mask is active, the canvas output rect
//! (xrSetSharedTextureOutputRectEXT) is SUPERSEDED, not an error: the weave
//! region, view dimensions, and projection metrics span the full client
//! window, and the mask is the sole 2D/3D region selector over that
//! window-spanning 3D scene. Destroying the mask restores the rect-derived
//! behavior on the next frame. (#439 Phase 2.)
XRAPI_ATTR XrResult XRAPI_CALL xrSubmitLocal3DZoneEXT(
    XrLocal3DZoneMaskEXT mask);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyLocal3DZoneMaskEXT(
    XrLocal3DZoneMaskEXT mask);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_EXT_LOCAL_3D_ZONE_H */
