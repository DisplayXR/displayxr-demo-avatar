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
 * @brief  Header for XR_DXR_view_rig extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * App-facing control of the runtime's view-rig math: the app chains a rig
 * descriptor (the handful of Kooima tunables) onto xrLocateViews and consumes
 * standard XrView{pose, fov} — render-ready, clip-independent — instead of
 * re-implementing the view math from raw eye positions. A separate raw-result
 * struct exposes the complete rig inputs (display-space eyes, display plane,
 * canvas rect, sample time, tracking lock) for aware consumers that keep doing
 * their own math (e.g. the WebXR bridge).
 *
 * Two rigs exist, matching the two canonical pipelines:
 *  - display rig (display-centric): the window/canvas is a portal into the
 *    scene; tunables = virtual display height (m2v scale), ipd factor,
 *    parallax factor, perspective factor.
 *  - camera rig (camera-centric): an app-defined camera whose frustum eye
 *    tracking perturbs; tunables = ipd factor, parallax factor, convergence,
 *    vertical FOV.
 *
 * Descriptors carry NO clip parameters (fov is clip-independent — near/far and
 * depth convention stay app-side) and NO placement parameters (the runtime owns
 * the window/canvas geometry). The rig is strictly per-locate: chain a
 * descriptor on every xrLocateViews you want it to drive (animating
 * convergence or virtual display height is just passing different values next
 * frame), and a locate that chains nothing keeps the default behavior —
 * including the raw-eye transport in XrView.pose for external-window apps.
 *
 * The app-facing halves are pure next-chain structs on xrLocateViews. The one
 * entry point, xrSetWorkspaceViewRigDXR, is for the workspace controller only:
 * an app's own rig is honored by default (app visual policy within its canvas),
 * but in workspace mode the controller may take over the clients' view geometry
 * through this call (e.g. forcing identity m2v during a layout animation). Full
 * design: docs/adr/ADR-024-raw-vs-render-ready-views.md (W7 of issue #396).
 */
#ifndef XR_DXR_VIEW_RIG_H
#define XR_DXR_VIEW_RIG_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_view_rig 1
#define XR_DXR_view_rig_SPEC_VERSION 1
#define XR_DXR_VIEW_RIG_EXTENSION_NAME "XR_DXR_view_rig"

// Reserved 1004999xxx range, next free block after mcp_tools (…130-132).
// Final values reconcile with the Khronos registry before spec freeze.
#define XR_TYPE_DISPLAY_RIG_DXR      ((XrStructureType)1004999140)
#define XR_TYPE_CAMERA_RIG_DXR       ((XrStructureType)1004999141)
#define XR_TYPE_VIEW_DISPLAY_RAW_DXR ((XrStructureType)1004999142)

//! Capacity of XrViewDisplayRawDXR::rawEyes (matches the runtime's max views).
#define XR_VIEW_RIG_MAX_RAW_EYES_DXR 8

// ---- Request: chain exactly ONE of these on XrViewLocateInfo::next. ----

/*!
 * @brief Display-centric rig: the window/canvas is a portal into the scene.
 *
 * Maps 1:1 onto the runtime's display-rig tunables. Out-of-range values are
 * clamped (with a one-shot runtime WARN), never rejected.
 */
typedef struct XrDisplayRigDXR {
    XrStructureType          type;   //!< Must be XR_TYPE_DISPLAY_RIG_DXR
    const void* XR_MAY_ALIAS next;
    XrPosef pose;                  //!< virtual display plane pose in the locate space
    float   virtualDisplayHeight;  //!< app units; m2v = this / physical canvas height
    float   ipdFactor;             //!< [0,1] multiplies view-pose spread about the center
    float   parallaxFactor;        //!< [0,1] lerps eye centroid toward nominal viewer
    float   perspectiveFactor;     //!< [0.1,10] scales eye XYZ (object perspective)
} XrDisplayRigDXR;

/*!
 * @brief Camera-centric rig: an app camera whose frustum eye tracking perturbs.
 *
 * Maps 1:1 onto the runtime's camera-rig tunables (convergenceDiopters is the
 * inverse convergence distance the math consumes; verticalFov converts to
 * half-tan at the boundary). Out-of-range values are clamped (one-shot WARN),
 * never rejected.
 *
 * metersToVirtual (spec v3) scales tracked head motion (meters) into the app's
 * world units in the modelview; with it, convergenceDiopters is the inverse
 * convergence distance in WORLD units (it reduces to 1/m when
 * metersToVirtual == 1). 0 (or unset) is treated as 1.0, preserving the
 * pre-v3 behavior where one world unit was implicitly one meter.
 */
typedef struct XrCameraRigDXR {
    XrStructureType          type;   //!< Must be XR_TYPE_CAMERA_RIG_DXR
    const void* XR_MAY_ALIAS next;
    XrPosef pose;                  //!< camera pose in the locate space
    float   ipdFactor;             //!< ABSOLUTE eye-separation scale (>= 0; any positive — a stereo
                                   //!< camera's inter-axial is not bounded to natural IPD). NOT the
                                   //!< display rig's relative [0,1] factor. Divergence comfort is
                                   //!< guarded on convergence, not by bounding this.
    float   parallaxFactor;        //!< ABSOLUTE parallax scale (>= 0); not the [0,1] display factor
    float   convergenceDiopters;   //!< 1/(convergence distance in world units); 0 = infinity
    float   verticalFov;           //!< radians, full vertical angle
    float   metersToVirtual;       //!< meters→world scale on the eye; 0/unset = 1.0 (spec v3)
} XrCameraRigDXR;

// ---- Result: app chains this on XrViewState::next; runtime fills it. ----

/*!
 * @brief Raw rig inputs for the locate, filled by the runtime.
 *
 * The complete input set the rigs consume, untransformed: per-view eye
 * positions verbatim in physical display space (NOT canvas-rebased — apply
 * the canvas rect yourself, e.g. via display3d_resolve_window_rect), the
 * display-plane pose, the effective canvas the runtime resolved for this
 * session, the eye-sample timestamp and the physical tracker lock. Reported
 * eyes are the DP's full per-view set verbatim — one eye per active view
 * (the display processor owns multi-view fill: e.g. it reports N eyes for an
 * N-view mode; the runtime never synthesizes eyes). When the tracker has no
 * lock the runtime still reports nominal-viewer eyes with isTracking = XR_FALSE.
 */
typedef struct XrViewDisplayRawDXR {
    XrStructureType    type;       //!< Must be XR_TYPE_VIEW_DISPLAY_RAW_DXR
    void* XR_MAY_ALIAS next;
    XrVector3f  rawEyes[XR_VIEW_RIG_MAX_RAW_EYES_DXR]; //!< display-space eye positions (meters,
                                   //!< display-center origin, +X right +Y up +Z toward viewer)
    uint32_t    eyeCountOutput;    //!< eyes written = the DP's per-view count
                                   //!< for this locate (one per active view)
    XrPosef     displayPlanePose;  //!< physical display plane in the locate space
    XrRect2Di   canvasRectPx;      //!< effective canvas on the panel (window client
                                   //!< area / texture sub-rect / runtime window)
    XrExtent2Df canvasSizeMeters;  //!< physical size of that canvas
    int64_t     sampleTimeNs;      //!< when the eyes were sampled (monotonic)
    XrBool32    isTracking;        //!< physical tracker lock (vs nominal fallback)
} XrViewDisplayRawDXR;

// ---- Entry point: workspace controller only. ----

// Impose a view rig on the workspace's app clients (or clear it). By default an
// app's own rig is honored within its canvas; while an override is set here, the
// runtime applies THIS rig to the clients' locates instead — the controller
// takes over view geometry (e.g. forcing identity m2v during a layout animation).
//
//   session — the active workspace controller's session (else
//             XR_ERROR_VALIDATION_FAILURE).
//   rig     — NULL, or an XrDisplayRigDXR / XrCameraRigDXR (its `type` selects
//             which); NULL clears the override so clients fall back to honoring
//             their own rigs. Out-of-range tunables are clamped.
//
// Per-call, not chained: the override stays in effect until changed or cleared.
typedef XrResult(XRAPI_PTR *PFN_xrSetWorkspaceViewRigDXR)(XrSession session, const void *rig);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceViewRigDXR(XrSession session, const void *rig);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_VIEW_RIG_H
