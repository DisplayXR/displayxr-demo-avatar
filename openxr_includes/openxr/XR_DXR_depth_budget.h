// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not. SPEC_VERSION continues the pre-rename XR_EXT_* numbering (the
// interface history did not restart with the name).
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_depth_budget extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * The REAR DEPTH BUDGET: how far behind the zero-disparity plane (ZDP, the
 * physical display plane) a transparent-background app may render before the
 * result reads as wrong.
 *
 * Transparent-mode apps composite over the live desktop. Content BEHIND the
 * ZDP carries positive disparity yet is drawn OVER desktop pixels that sit at
 * zero disparity — an occlusion-vs-disparity conflict. Today those apps avoid
 * it by hard-clipping their far plane at the ZDP, which throws away all rear
 * depth unconditionally.
 *
 * The conflict is only perceptible where the background behind the app carries
 * a HORIZONTAL-disparity cue: horizontal luminance gradients (vertical edges,
 * text, icons, window borders). A uniform — or merely horizontally uniform —
 * background (vertical gradients, horizontal stripes) carries no cue, so rear
 * content is perceptually fine over it.
 *
 * Division of labour: the RUNTIME owns the policy (it watches the background
 * the display processor already captures and decides how much rear depth is
 * safe), the DISPLAY PROCESSOR owns pixels, and the APP owns geometry — it
 * applies the advisory budget as its far-plane offset and does no analysis of
 * its own.
 *
 * Units: vH, virtual display heights — the far-offset convention the transparent
 * demos already use. 0 = clip at the ZDP (today's behaviour); >= 1000 =
 * unrestricted. farOffsetMeters is the same number pre-multiplied by the rig's
 * virtual display height, for convenience.
 *
 * The budget is a RAMPED value: the runtime slides it rather than stepping it,
 * so the app's clip plane glides. Apps apply what they are handed verbatim and
 * must NOT add smoothing of their own. The state (not the ramp) changes rarely,
 * and each change is also delivered as an event.
 *
 * Enabling the extension is the app's opt-in: the runtime only runs the
 * background fetch + analysis for sessions that enabled it, are transparent,
 * and are standalone (a session running under a workspace controller has no
 * live desktop behind it and is UNRESTRICTED_WORKSPACE).
 */
#ifndef XR_DXR_DEPTH_BUDGET_H
#define XR_DXR_DEPTH_BUDGET_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_depth_budget 1
#define XR_DXR_depth_budget_SPEC_VERSION 3
#define XR_DXR_DEPTH_BUDGET_EXTENSION_NAME "XR_DXR_depth_budget"

// Reserved 1004999xxx range, next free block after view_rig (…140-142).
// Final values reconcile with the Khronos registry before spec freeze.
#define XR_TYPE_REAR_DEPTH_BUDGET_DXR ((XrStructureType)1004999260)
//! App-reported content bounds, narrowing the analysis ROI (v2).
#define XR_TYPE_CONTENT_BOUNDS_DXR ((XrStructureType)1004999261)
#define XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR ((XrStructureType)1004999262)
//! App-reported content occupancy mask (v3) - the silhouette, not a box.
#define XR_TYPE_CONTENT_MASK_DXR ((XrStructureType)1004999263)

/*!
 * @brief Why the runtime is handing out the budget it is handing out.
 *
 * Kept in sync with the runtime's internal policy state machine
 * (`u_rear_budget_state`).
 */
typedef enum XrRearDepthBudgetStateDXR {
    //! Session is not transparent — nothing composites over the desktop.
    XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_OPAQUE_DXR = 0,
    //! Transparent, but running under a workspace controller (today's behaviour).
    XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_WORKSPACE_DXR = 1,
    //! Transparent + standalone, background carries no horizontal cue.
    XR_REAR_DEPTH_BUDGET_STATE_OPEN_DXR = 2,
    //! Transparent + standalone, background is busy — ramping shut.
    XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_BUSY_BACKGROUND_DXR = 3,
    //! No background preview available (no capture, client-present, stale).
    XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_NO_SOURCE_DXR = 4,
    //! An environment override is pinning the budget open or shut.
    XR_REAR_DEPTH_BUDGET_STATE_FORCED_DXR = 5,
    XR_REAR_DEPTH_BUDGET_STATE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrRearDepthBudgetStateDXR;

// ---- Result: app chains this on XrViewState::next; runtime fills it. ----

/*!
 * @brief The advisory rear depth budget for this locate, filled by the runtime.
 *
 * Chain beside @ref XrViewDisplayRawDXR on XrViewState::next in xrLocateViews;
 * the runtime fills it on every locate. When the runtime has nothing yet it
 * still writes the conservative default — farOffsetVH = 0 for a transparent
 * session, 1000 otherwise — so an app never has to distinguish "not filled"
 * from "clipped".
 *
 * This is a SEPARATE struct rather than new fields on @ref XrViewDisplayRawDXR
 * deliberately: growing that struct would let the runtime write past the end of
 * the allocation an app compiled against an older spec version handed it.
 */
typedef struct XrRearDepthBudgetDXR {
    XrStructureType    type;   //!< Must be XR_TYPE_REAR_DEPTH_BUDGET_DXR
    void* XR_MAY_ALIAS next;
    float                     farOffsetVH;         //!< >= 0; 0 = clip at ZDP, >= 1000 = unrestricted
    float                     farOffsetMeters;     //!< farOffsetVH * virtual display height (0 if rig unknown)
    XrRearDepthBudgetStateDXR state;               //!< Why this value
    float                     backgroundCueEnergy; //!< 0..1 diagnostic; 0 when there is no source
} XrRearDepthBudgetDXR;

// ---- Input: app chains this on XrFrameEndInfo::next; runtime reads it. ----

/*!
 * @brief Where this frame's content actually is, so the runtime measures only
 *        the background it will be drawn over (v2).
 *
 * v1 judged the whole canvas: a busy patch anywhere under the app closed the
 * budget for the whole session, even when the model occupied one corner and
 * the busy pixels were nowhere near it. The conflict is local — it exists only
 * where rear content overlaps a horizontal cue — so the app, which is the only
 * party that knows where its geometry lands, says so.
 *
 * Chain this on XrFrameEndInfo::next in xrEndFrame. It is OPTIONAL and purely
 * advisory: an app that never chains it (or stops chaining it for more than a
 * second) gets the v1 behaviour, the whole canvas. A malformed value is never
 * an error — it degrades to "unknown", which is again the whole canvas. This
 * struct is an input only; nothing is written back through it.
 *
 * The runtime DILATES the region before measuring, because the disparity
 * conflict lives in the band around the silhouette rather than strictly under
 * it. @ref marginNormalized is dilation the app asks for ON TOP of the
 * runtime's own default.
 *
 * The runtime also CLAMPS the region to the frame's 3D display zones. Outside
 * a 3D zone nothing is woven, so measuring the desktop there answers a question
 * about pixels the content can never occlude.
 */
typedef struct XrContentBoundsDXR {
    XrStructureType          type;   //!< Must be XR_TYPE_CONTENT_BOUNDS_DXR
    const void* XR_MAY_ALIAS next;
    /*!
     * WINDOW-NORMALISED: offset/extent in [0,1], origin top-left (u right, v
     * DOWN). The frame these normalise to is the APP WINDOW'S CLIENT RECT —
     * the frame of the display processor's background preview — and NEVER a
     * display zone's `canvasRectPx`.
     *
     * The union over ALL views of the projected content AABB. An extent <= 0,
     * or any non-finite component, means "unknown" and the runtime measures the
     * whole window.
     *
     * **A zoned app must rebase.** With XR_DXR_display_zones the app projects
     * in the ZONE's view, clamps to [0,1] of the zone, and then rebases through
     * that zone's window rect before reporting — `dxr::ProjectAabbToWindowBounds`
     * / `dxr::RebaseZoneBoundsToWindow` in `displayxr-common` do both halves.
     * Chaining zone-normalised bounds as if they were window-normalised aims the
     * analysis at the wrong part of the window, typically at a Local2D 2D band
     * the content never covers. The runtime additionally clamps the region to
     * the frame's 3D display zones, so that mistake degrades to a coarser
     * measurement rather than a wrong one — but the clamp is a defence, not the
     * contract.
     */
    XrRect2Df bounds;
    //! Extra dilation the app wants, in window-normalised units. 0 = the runtime default alone.
    float marginNormalized;
} XrContentBoundsDXR;

// ---- Event: emitted on every STATE change (never on ramp progress). ----

/*!
 * @brief The session's rear-depth-budget state changed.
 *
 * The budget value itself ramps continuously and is read per-locate from
 * @ref XrRearDepthBudgetDXR; this event fires only when the reason changes, so
 * an app can log it or drive UI without polling.
 */
typedef struct XrEventDataRearDepthBudgetStateChangedDXR {
    XrStructureType    type;   //!< Must be XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                 session;
    XrRearDepthBudgetStateDXR previousState;
    XrRearDepthBudgetStateDXR newState;
} XrEventDataRearDepthBudgetStateChangedDXR;

/*!
 * v3 INPUT (SPEC_VERSION 3): the app's content OCCUPANCY MASK for this frame - the
 * union over ALL views of its rendered silhouette (the same artefact a transparent
 * app derives from alpha for its click-through window region). Chain on
 * XrFrameEndInfo::next, beside or instead of XrContentBoundsDXR.
 *
 * Grid: row-major, top-left origin, WINDOW-CLIENT-normalised extent - cell (x, y)
 * covers [x/width, (x+1)/width) x [y/height, (y+1)/height) of the window client
 * rect (the same frame as XrContentBoundsDXR; a zoned app writes its zone's
 * silhouette into the window grid and leaves the rest 0). nonzero = content
 * occupies the cell. The runtime copies the cells during xrEndFrame; the pointer
 * need only stay valid until xrEndFrame returns.
 *
 * Precedence in the runtime, most specific first, each falling back to the next
 * when absent, all-zero or stale (> 1 s): mask -> XrContentBoundsDXR -> the 3D
 * display zones -> the whole canvas. The runtime resamples the mask onto its
 * own analysis grid with an any-coverage filter, clamps it to the 3D zones and
 * dilates it by the disparity band before measuring; only masked pixels count.
 */
typedef struct XrContentMaskDXR {
    XrStructureType          type;   //!< Must be XR_TYPE_CONTENT_MASK_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 width;        //!< 1..512 cells
    uint32_t                 height;       //!< 1..512 cells
    uint32_t                 strideBytes;  //!< >= width
    const uint8_t*           cells;        //!< width x height bytes, nonzero = occupied
    //! Extra dilation the app wants, in window-normalised units. 0 = the runtime default alone.
    float                    marginNormalized;
} XrContentMaskDXR;

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_DEPTH_BUDGET_H
