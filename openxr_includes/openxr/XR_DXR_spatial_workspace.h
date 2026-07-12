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
 * @brief  Header for XR_DXR_spatial_workspace extension (Phase 2.A subset)
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Public surface for a workspace controller — a privileged OpenXR client that
 * drives session lifecycle and 2D OS-window capture for a spatial workspace.
 *
 * Phase 2.A scope: lifecycle (activate/deactivate/get-state) and the capture-
 * client cluster (add/remove). The full surface (window pose, hit-test, frame
 * capture, client enumeration, lifecycle events) lands in subsequent sub-
 * phases. See docs/roadmap/spatial-workspace-extensions-headers-draft.md for
 * the complete API sketch.
 */
#ifndef XR_DXR_SPATIAL_WORKSPACE_H
#define XR_DXR_SPATIAL_WORKSPACE_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_spatial_workspace 1
#define XR_DXR_spatial_workspace_SPEC_VERSION 1
#define XR_DXR_SPATIAL_WORKSPACE_EXTENSION_NAME "XR_DXR_spatial_workspace"

// Provisional XrStructureType values. The 1004999100..110 range is reserved for
// this extension; final values reconcile with the Khronos registry before spec
// freeze.
#define XR_TYPE_WORKSPACE_CLIENT_INFO_DXR                   ((XrStructureType)1004999100)
#define XR_TYPE_WORKSPACE_CAPTURE_REQUEST_DXR               ((XrStructureType)1004999101)
#define XR_TYPE_WORKSPACE_CAPTURE_RESULT_DXR                ((XrStructureType)1004999102)
#define XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_DXR  ((XrStructureType)1004999103)
#define XR_TYPE_WORKSPACE_CHROME_LAYOUT_DXR                 ((XrStructureType)1004999104)
#define XR_TYPE_WORKSPACE_CLIENT_STYLE_DXR                  ((XrStructureType)1004999105)
#define XR_TYPE_WORKSPACE_CURSOR_SWAPCHAIN_CREATE_INFO_DXR  ((XrStructureType)1004999106)  // spec_version 13
#define XR_TYPE_WORKSPACE_CURSOR_INFO_DXR                   ((XrStructureType)1004999107)  // spec_version 13
#define XR_TYPE_WORKSPACE_OVERLAY_SWAPCHAIN_CREATE_INFO_DXR ((XrStructureType)1004999108)  // spec_version 17
#define XR_TYPE_WORKSPACE_OVERLAY_INFO_DXR                  ((XrStructureType)1004999109)  // spec_version 17
#define XR_TYPE_WORKSPACE_CURSOR_DEPTH_DXR                  ((XrStructureType)1004999110)  // spec_version 22

/*!
 * @brief Workspace-local identifier for a client.
 *
 * Stable for the lifetime of the connection; reused after disconnect. 0 is
 * reserved (invalid) and represented by XR_NULL_WORKSPACE_CLIENT_ID.
 */
typedef uint32_t XrWorkspaceClientId;

#define XR_NULL_WORKSPACE_CLIENT_ID ((XrWorkspaceClientId)0)

/*!
 * @brief Controller-defined opaque identifier for a chrome hit region.
 *
 * The workspace controller assigns these when populating
 * XrWorkspaceChromeLayoutDXR::hitRegions. The runtime echoes the matching
 * region's id back on POINTER / POINTER_MOTION events when a ray-cast lands
 * inside the chrome quad. 0 (XR_NULL_WORKSPACE_CHROME_REGION_ID) means "no
 * chrome region hit" — either the hit was on content / background or the
 * controller chose to leave the region anonymous.
 */
typedef uint32_t XrWorkspaceChromeRegionIdDXR;

#define XR_NULL_WORKSPACE_CHROME_REGION_ID ((XrWorkspaceChromeRegionIdDXR)0)

/*!
 * @brief Type of a workspace client.
 *
 * OpenXR clients connect via xrCreateSession; captured-2D clients are adopted
 * via xrAddWorkspaceCaptureClientDXR. The workspace controller cannot tell
 * them apart from the enumeration result except via this enum.
 */
typedef enum XrWorkspaceClientTypeDXR {
    XR_WORKSPACE_CLIENT_TYPE_OPENXR_3D_DXR   = 0,
    XR_WORKSPACE_CLIENT_TYPE_CAPTURED_2D_DXR = 1,
    XR_WORKSPACE_CLIENT_TYPE_MAX_ENUM_DXR    = 0x7FFFFFFF
} XrWorkspaceClientTypeDXR;

/*!
 * @brief Classification of a spatial hit-test result.
 *
 * Phase 2.D established hit-region as the shared vocabulary between runtime
 * and workspace controller: the runtime classifies (it has the geometry) and
 * the controller interprets (focus, drag, close, etc.). The taskbar and
 * launcher tile regions are produced by the launcher hit-test path; window
 * raycasts produce the rest.
 */
typedef enum XrWorkspaceHitRegionDXR {
    XR_WORKSPACE_HIT_REGION_BACKGROUND_DXR       = 0,  // miss
    XR_WORKSPACE_HIT_REGION_CONTENT_DXR           = 1,
    XR_WORKSPACE_HIT_REGION_TITLE_BAR_DXR         = 2,
    XR_WORKSPACE_HIT_REGION_CLOSE_BUTTON_DXR      = 3,  // legacy; spec_version <= 6
    XR_WORKSPACE_HIT_REGION_MINIMIZE_BUTTON_DXR   = 4,  // legacy; spec_version <= 6
    XR_WORKSPACE_HIT_REGION_MAXIMIZE_BUTTON_DXR   = 5,  // legacy; spec_version <= 6
    XR_WORKSPACE_HIT_REGION_CHROME_DXR            = 6,  // controller-owned chrome (spec_version 7)
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_N_DXR     = 10,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_S_DXR     = 11,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_E_DXR     = 12,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_W_DXR     = 13,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_NE_DXR    = 14,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_NW_DXR    = 15,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_SE_DXR    = 16,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_SW_DXR    = 17,
    XR_WORKSPACE_HIT_REGION_TASKBAR_DXR           = 20,
    XR_WORKSPACE_HIT_REGION_LAUNCHER_TILE_DXR     = 21,
    XR_WORKSPACE_HIT_REGION_MAX_ENUM_DXR          = 0x7FFFFFFF
} XrWorkspaceHitRegionDXR;

// ---- Lifecycle ----

/*!
 * @brief Activate workspace mode on this session.
 *
 * The session becomes the privileged workspace controller. At most one
 * workspace is active per system. Caller authorization is via orchestrator-
 * PID match (with a manual-mode fallback when no orchestrator-spawned
 * controller is registered).
 *
 * @param session A valid XrSession with XR_DXR_spatial_workspace enabled.
 * @return XR_SUCCESS on success,
 *         XR_ERROR_LIMIT_REACHED if another workspace is already active,
 *         XR_ERROR_FEATURE_UNSUPPORTED if the caller is not authorized,
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the extension was not enabled.
 */
typedef XrResult (XRAPI_PTR *PFN_xrActivateSpatialWorkspaceDXR)(XrSession session);

/*!
 * @brief Voluntarily release the workspace role.
 *
 * Other clients keep running; per-client compositors resume direct rendering.
 * xrDestroySession on the workspace session has the same effect implicitly.
 */
typedef XrResult (XRAPI_PTR *PFN_xrDeactivateSpatialWorkspaceDXR)(XrSession session);

/*!
 * @brief Query whether this session is currently the active workspace.
 *
 * @param session    A valid XrSession with XR_DXR_spatial_workspace enabled.
 * @param out_active Output: XR_TRUE if this session holds the workspace role.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetSpatialWorkspaceStateDXR)(
    XrSession session, XrBool32 *out_active);

// ---- Capture clients (adopt a 2D OS window) ----

/*!
 * @brief Adopt a 2D OS window as a workspace client.
 *
 * The runtime starts a platform-appropriate capture (Windows.Graphics.Capture
 * on Windows; macOS path lands in a follow-up sub-phase) and treats the
 * captured texture as a client swapchain — the workspace can position/hide it
 * like any other client.
 *
 * @param session       A valid workspace session.
 * @param nativeWindow  Windows: HWND cast to uint64_t. macOS: 0 + chained
 *                      XrWorkspaceCocoaCaptureBindingDXR (post-2.A).
 * @param nameOptional  User-visible label, may be NULL. Currently advisory —
 *                      will be propagated through IPC in a later sub-phase.
 * @param outClientId   Output: XrWorkspaceClientId for the adopted window.
 *                      Enters the same numbering space as OpenXR clients.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAddWorkspaceCaptureClientDXR)(
    XrSession            session,
    uint64_t             nativeWindow,
    const char          *nameOptional,
    XrWorkspaceClientId *outClientId);

/*!
 * @brief Release a previously adopted 2D OS-window capture client.
 *
 * Stops the capture, removes the client from the workspace, and recycles the
 * client id.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRemoveWorkspaceCaptureClientDXR)(
    XrSession           session,
    XrWorkspaceClientId clientId);

// ---- Window pose + visibility (spec_version 2) ----

/*!
 * @brief Set position, orientation, and physical size of a client's window.
 *
 * The runtime composites the named client's swapchain into a quad at this
 * pose; the next frame reflects the change. Pose origin is the display
 * center; +x right, +y up, +z toward the viewer. Width and height are the
 * quad's physical extent in meters — the runtime stretches the client's
 * atlas to fit.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_VALIDATION_FAILURE if clientId is unknown or not a
 *         positionable client (some platform-internal clients may be
 *         excluded), XR_ERROR_FEATURE_UNSUPPORTED if the session is not
 *         the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientWindowPoseDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    const XrPosef       *pose,
    float                widthMeters,
    float                heightMeters);

/*!
 * @brief Read back the current pose and physical size of a client's window.
 *
 * Useful for controllers that need to preserve relative layout when one
 * client moves, or for restoring saved workspaces. All output pointers
 * must be non-NULL.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceClientWindowPoseDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrPosef             *outPose,
    float               *outWidthMeters,
    float               *outHeightMeters);

/*!
 * @brief Show or hide a client's window without destroying it.
 *
 * A hidden client keeps running but does not contribute to the composite —
 * standard "minimize" semantics. The client may continue rendering frames
 * that the runtime drops, or its render thread may pause; that is a
 * client-side concern. xrGetSpatialWorkspaceClientVisibilityDXR (future)
 * would expose readback; Phase 2.C ships set-only.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientVisibilityDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             visible);

// ---- Per-client frame-rate cap (spec_version 14) ----

/*!
 * @brief Cap a workspace client's xrWaitFrame return cadence.
 *
 * Pure mechanism. The runtime stores @p maxFps per client and applies
 * @c multiplier = max(1, round(refresh_rate_hz / maxFps)) to the
 * out_wake_time_ns and out_predicted_display_period_ns returned by the
 * IPC predict_frame path. The client sleeps client-side; no server
 * thread is parked. Effective only for IPC-mode (workspace) clients —
 * in-process compositor consumers (e.g. WebXR bridge) are not affected.
 *
 * Policy is the controller's. A typical workspace controller might call
 *   xrSetWorkspaceClientFrameRateCapDXR(session, prev_focus, 30.0f);
 *   xrSetWorkspaceClientFrameRateCapDXR(session, new_focus, 0.0f);
 * on every focus change, and 1.0f for minimized clients. Different
 * workspaces (picture-in-picture, live thumbnails, accessibility) can
 * pick different cadences without runtime changes.
 *
 * @param session   A valid workspace session.
 * @param clientId  The client to cap. XR_NULL_WORKSPACE_CLIENT_ID is invalid.
 * @param maxFps    Maximum xrWaitFrame return rate in Hz. 0.0f means
 *                  uncapped (native refresh). Negative values are
 *                  rejected with XR_ERROR_VALIDATION_FAILURE.
 *
 * The cap auto-resets to 0.0f on client disconnect; controllers must
 * re-apply on reconnect. The runtime never lowers the cap on its own.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientFrameRateCapDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    float                maxFps);

// ---- Hit-test (removed in spec_version 22) ----
//
// xrWorkspaceHitTestDXR and its per-frame raycast were removed in spec_version 22.
// The workspace controller now owns hit-testing entirely (eye→cursor ray vs.
// window quads, region classification) and feeds the runtime only the resulting
// cursor depth via xrSetWorkspaceCursorDepthDXR (see below). XrWorkspaceHitRegionDXR
// is retained because POINTER / POINTER_MOTION events still carry a hitRegion the
// controller fills in.

// ---- Focus control (spec_version 4) ----

/*!
 * @brief Set the workspace's focused client.
 *
 * The runtime forwards keyboard input (other than chords the controller has
 * reserved via xrSetWorkspaceReservedKeysDXR) and click-through events to the
 * focused client's HWND. The workspace controller decides who gets focus from
 * its interpretation of pointer events drained via
 * xrEnumerateWorkspaceInputEventsDXR.
 *
 * @param session   A valid workspace session.
 * @param clientId  The client to focus, or XR_NULL_WORKSPACE_CLIENT_ID
 *                  to clear focus (no client receives forwarded input).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceFocusedClientDXR)(
    XrSession           session,
    XrWorkspaceClientId clientId);

/*!
 * @brief Read the workspace's currently focused client.
 *
 * @param session       A valid workspace session.
 * @param outClientId   Output: the focused clientId, or
 *                      XR_NULL_WORKSPACE_CLIENT_ID if no client is focused.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceFocusedClientDXR)(
    XrSession            session,
    XrWorkspaceClientId *outClientId);

// ---- Reserved keys (spec_version 24) ----

/*!
 * @brief One reserved key chord the controller owns.
 *
 * A key event matches an entry only when BOTH its vkCode and its modifier
 * mask equal the entry exactly. The mask uses the same 3-bit convention as
 * the KEY input event (bit0=SHIFT, bit1=CTRL, bit2=ALT), so {VK_TAB, 0}
 * reserves bare Tab while {VK_TAB, SHIFT} is a distinct, unreserved chord
 * that still forwards to the focused client.
 */
typedef struct XrWorkspaceReservedKeyDXR {
    uint32_t vkCode;     // Win32 VK_*
    uint32_t modifiers;  // bit0=SHIFT, bit1=CTRL, bit2=ALT
} XrWorkspaceReservedKeyDXR;

#define XR_WORKSPACE_MAX_RESERVED_KEYS_DXR 32

/*!
 * @brief Declare the set of key chords the controller reserves.
 *
 * Reserved chords are still emitted on the input-event queue (so the
 * controller can act on them) but are NOT forwarded to the focused client's
 * HWND. This replaces the runtime's hardcoded reserved-key policy: the
 * controller is the single source of truth for which chords it owns.
 *
 * Matching is exact on (vkCode, modifiers) — see XrWorkspaceReservedKeyDXR.
 * Non-reserved key chords are forwarded to the focused client with their
 * modifier state preserved.
 *
 * @param session   A valid workspace session.
 * @param keyCount  Number of entries in @p keys (0 restores the runtime's
 *                  built-in default set; at most
 *                  XR_WORKSPACE_MAX_RESERVED_KEYS_DXR).
 * @param keys      Array of reserved chords; may be NULL when keyCount == 0.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_VALIDATION_FAILURE if keyCount exceeds the max or keys is
 *         NULL with keyCount > 0,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceReservedKeysDXR)(
    XrSession                        session,
    uint32_t                         keyCount,
    const XrWorkspaceReservedKeyDXR *keys);

// ---- Input event drain + pointer capture (spec_version 4) ----

typedef enum XrWorkspaceInputEventTypeDXR {
    XR_WORKSPACE_INPUT_EVENT_POINTER_DXR        = 0,
    XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_DXR  = 1, // spec_version 7: emitted on hovered-slot transitions (per-frame raycast — fires regardless of pointer-capture state)
    XR_WORKSPACE_INPUT_EVENT_KEY_DXR            = 2,
    XR_WORKSPACE_INPUT_EVENT_SCROLL_DXR         = 3,
    XR_WORKSPACE_INPUT_EVENT_POINTER_MOTION_DXR = 4, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_FRAME_TICK_DXR     = 5, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED_DXR  = 6, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED_DXR = 7, // spec_version 8: runtime-driven pose / size change (edge resize, etc.)
    XR_WORKSPACE_INPUT_EVENT_MODAL_OPEN_DXR      = 8, // spec_version 10: client opened a Win32 modal popup (refcounted, fires on 0→1 only)
    XR_WORKSPACE_INPUT_EVENT_MODAL_CLOSE_DXR     = 9, // spec_version 10: client's last Win32 modal popup closed (refcounted, fires on 1→0 only)
    XR_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST_DXR = 10, // spec_version 11: a workspace client called xrRequestFilePickerDXR — fetch full info via xrGetFilePickerRequestDXR, deliver result via xrCompleteFilePickerDXR
    XR_WORKSPACE_INPUT_EVENT_FULLSCREEN_TOGGLED_DXR  = 11, // spec_version 15: a workspace client's fullscreen/maximize state transitioned (runtime-driven, e.g. double-click title bar or F11)
    XR_WORKSPACE_INPUT_EVENT_CLIENT_CONNECTED_DXR    = 12, // spec_version 16: a client connected (slot bound). The controller owns ALL per-client setup — initial pose (xrSetWorkspaceClientWindowPoseDXR), and over time chrome / style / focus / entry animation. The runtime does not draw the client until the controller has placed it (set_pose), so there is no flash regardless of controller latency.
    XR_WORKSPACE_INPUT_EVENT_TYPE_MAX_ENUM_DXR  = 0x7FFFFFFF
} XrWorkspaceInputEventTypeDXR;

/*!
 * @brief One input event drained from the workspace input queue.
 *
 * Tagged C union — `eventType` selects the meaningful union member. Pointer
 * events carry the cursor position (cursorX / cursorY in display pixels); as of
 * spec_version 22 the controller owns hit-testing and fills in the
 * clientId / region / UV fields itself from its own eye→cursor raycast (the
 * runtime no longer computes them at drain time).
 *
 * Key forwarding policy (spec_version 24): the controller declares the chords
 * it reserves via xrSetWorkspaceReservedKeysDXR. Reserved chords are emitted
 * on this queue but NOT forwarded to the focused HWND; every other chord is
 * both emitted here AND forwarded to the focused client with its modifier
 * state preserved. Until the controller registers a set, the runtime falls
 * back to a built-in default (bare TAB/DELETE/ESC/[/]).
 *
 * Pointer motion events (spec_version 6) deliver per-frame WM_MOUSEMOVE while
 * pointer capture is enabled. Controllers wanting hover feedback without
 * holding a button can opt in via xrEnableWorkspacePointerCaptureDXR (any
 * button) and run their own hit-test over the per-frame cursor positions.
 *
 * Frame-tick events (spec_version 6) fire once per compositor frame so a
 * controller can pace per-frame work (animation interpolation, hover effects)
 * to display refresh without polling a timer.
 *
 * Focus-changed events (spec_version 6) fire when the runtime's focused client
 * transitions (TAB cycle, click auto-focus, controller-set, client disconnect).
 * They do NOT fire on every frame when focus is stable.
 */
typedef struct XrWorkspaceInputEventDXR {
    XrWorkspaceInputEventTypeDXR eventType;
    uint32_t                     timestampMs;        // host monotonic ms (low 32 bits)
    union {
        struct {
            XrWorkspaceClientId          hitClientId;
            XrWorkspaceHitRegionDXR      hitRegion;
            XrVector2f                   localUV;
            int32_t                      cursorX;          // display pixels, top-left origin
            int32_t                      cursorY;
            uint32_t                     button;            // 1=L, 2=R, 3=M
            XrBool32                     isDown;
            uint32_t                     modifiers;         // bit0=SHIFT, bit1=CTRL, bit2=ALT
            XrWorkspaceChromeRegionIdDXR chromeRegionId;    // spec_version 7: controller-defined region within chrome quad, 0 if none
        } pointer;
        struct {  // hovered-slot transitions; spec_version 9 adds chromeRegionId
            XrWorkspaceClientId          prevClientId;
            XrWorkspaceHitRegionDXR      prevRegion;
            XrWorkspaceClientId          currentClientId;
            XrWorkspaceHitRegionDXR      currentRegion;
            // spec_version 9: controller-defined chrome region the cursor is
            // over (matches the chromeRegionId field on POINTER / POINTER_MOTION
            // events; 0 = none). Lets the shell drive per-region UI feedback —
            // button hover-lighten, tooltip popovers, etc. — without enabling
            // pointer capture continuously. The runtime fires a POINTER_HOVER
            // whenever EITHER the hovered slot OR the resolved chromeRegionId
            // changes, so a cursor moving from grip → close inside the same
            // window's chrome bar still produces a transition event.
            XrWorkspaceChromeRegionIdDXR prevChromeRegionId;
            XrWorkspaceChromeRegionIdDXR currentChromeRegionId;
        } pointerHover;
        struct {
            uint32_t                vkCode;          // Win32 VK_* (cross-platform mapping TBD)
            XrBool32                isDown;
            uint32_t                modifiers;
        } key;
        struct {
            float                   deltaY;          // wheel ticks (+ = up)
            int32_t                 cursorX;
            int32_t                 cursorY;
            uint32_t                modifiers;
        } scroll;
        struct {  // spec_version 6: per-frame mouse motion (capture-gated)
            XrWorkspaceClientId          hitClientId;
            XrWorkspaceHitRegionDXR      hitRegion;
            XrVector2f                   localUV;
            int32_t                      cursorX;          // display pixels, top-left origin
            int32_t                      cursorY;
            uint32_t                     buttonMask;        // bit0=L, bit1=R, bit2=M (currently held)
            uint32_t                     modifiers;         // bit0=SHIFT, bit1=CTRL, bit2=ALT
            XrWorkspaceChromeRegionIdDXR chromeRegionId;    // spec_version 7: controller-defined region within chrome quad, 0 if none
        } pointerMotion;
        struct {  // spec_version 6: vsync-aligned compositor frame tick
            uint64_t                timestampNs;     // host monotonic ns at frame compose
            XrVector3f              viewerPosInDisplaySpace; // spec_version 20: viewer eye-midpoint, display space (m)
            XrBool32                viewerTracked;   // spec_version 20: 1 if viewerPos is populated this frame
            int32_t                 cursorX;         // spec_version 22: OS cursor X, display pixels, top-left origin
            int32_t                 cursorY;         // spec_version 22: OS cursor Y. Controller runs its own hit-test over this per frame.
        } frameTick;
        struct {  // spec_version 6: workspace focused-client transition
            XrWorkspaceClientId     prevClientId;
            XrWorkspaceClientId     currentClientId;
        } focusChanged;
        struct {  // spec_version 8: window pose/size changed by the runtime
            // (edge resize, fullscreen toggle, etc. — NOT shell-driven
            // set_pose RPCs, which the shell already knows about).
            // Controllers re-push chrome layout in response so chrome
            // tracks the window's new top edge instead of staying at the
            // old size.
            XrWorkspaceClientId     clientId;
            XrPosef                 pose;
            float                   widthMeters;
            float                   heightMeters;
        } windowPoseChanged;
        struct {  // spec_version 10: Win32 modal popup state. Refcounted
            // runtime-side across nested popups, so the controller sees
            // exactly one MODAL_OPEN on 0→1 and one MODAL_CLOSE on 1→0
            // per logical modal session, regardless of nesting depth.
            // Companion to ADR-017 (Tier 0 modal-dialog strategy).
            XrWorkspaceClientId     clientId;
        } modal;
        struct {  // spec_version 11: a workspace client called
            // xrRequestFilePickerDXR (XR_DXR_workspace_file_dialog).
            // Payload is intentionally small so the event batch stays
            // under IPC_BUF_SIZE; the controller calls
            // xrGetFilePickerRequestDXR(requestId, …) to retrieve the
            // full XrFilePickerInfoDXR-equivalent, spawns its picker
            // implementation, and delivers the result via
            // xrCompleteFilePickerDXR.
            XrWorkspaceClientId     clientId;
            uint64_t                requestId;
        } filePickerRequest;
        struct {  // spec_version 15: a client's fullscreen/maximize state
            // transitioned. Runtime-driven (double-click title bar, F11,
            // xrRequestWorkspaceClientFullscreenDXR). The workspace
            // controller typically wants to focus the toggled client when
            // it enters fullscreen — focus policy lives in the controller
            // (ADR-018), so the runtime emits the event and the controller
            // calls xrSetWorkspaceFocusedClientDXR in response.
            XrWorkspaceClientId     clientId;
            XrBool32                isFullscreen;  // XR_TRUE on enter, XR_FALSE on restore
        } fullscreenToggled;
        struct {  // spec_version 16: a client connected and its slot is
            // bound. The runtime no longer owns per-client policy (ADR-018):
            // the controller responds by placing the client
            // (xrSetWorkspaceClientWindowPoseDXR) and, per its own design,
            // creating chrome, pushing style, choosing focus, and driving the
            // entry animation. The runtime will NOT composite the client
            // until the controller has placed it (first set_pose) AND it has
            // committed a frame — so the controller's latency never produces
            // a flash, and set_pose on this event is race-free (the slot is
            // already bound when the event fires).
            XrWorkspaceClientId     clientId;
        } clientConnected;
    };
} XrWorkspaceInputEventDXR;

/*!
 * @brief Drain pending workspace input events into a controller-supplied buffer.
 *
 * Up to @p capacityInput events are consumed from the workspace input queue
 * and written into @p events. The runtime returns the actual number written
 * via @p countOutput. Events are destructive — once drained, they will not
 * appear on a subsequent call.
 *
 * Maximum events per RPC is implementation-defined and bounded by
 * IPC_BUF_SIZE; pass at most 16 in @p capacityInput in the current
 * implementation. A capacity of 0 returns 0 without draining.
 *
 * @param session       A valid workspace session.
 * @param capacityInput The maximum number of events to drain.
 * @param countOutput   Output: number of events actually written.
 * @param events        Output array; may be NULL when capacityInput == 0.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateWorkspaceInputEventsDXR)(
    XrSession                  session,
    uint32_t                   capacityInput,
    uint32_t                  *countOutput,
    XrWorkspaceInputEventDXR  *events);

/*!
 * @brief Begin pointer capture so events for @p button keep flowing even when
 * the cursor leaves any window.
 *
 * Pair with xrDisableWorkspacePointerCaptureDXR to release. Used to
 * implement controller-driven drag affordances (move, resize) without the
 * runtime needing to know about drag policy.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnableWorkspacePointerCaptureDXR)(
    XrSession session,
    uint32_t  button);

typedef XrResult (XRAPI_PTR *PFN_xrDisableWorkspacePointerCaptureDXR)(
    XrSession session);

// ---- Lifecycle requests (spec_version 6) ----

/*!
 * @brief Ask the runtime to close a specific workspace client.
 *
 * Equivalent to the runtime's built-in DELETE shortcut, but targeted at any
 * client (not just the focused one). For OpenXR clients the runtime emits
 * XRT_SESSION_EVENT_EXIT_REQUEST so the client exits cleanly; for capture
 * clients the runtime tears down the capture immediately.
 *
 * The controller can drive this from its own chrome (a custom close button on
 * a window decoration, an overview gesture, etc.) without the runtime needing
 * to know about that policy.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestWorkspaceClientExitDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId);

/*!
 * @brief Ask the runtime to toggle fullscreen for a specific workspace client.
 *
 * When @p fullscreen is XR_TRUE the target window animates to fill the display
 * and other clients hide; XR_FALSE restores the prior layout. Mirrors the
 * runtime's built-in F11 shortcut, but targeted at any client.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestWorkspaceClientFullscreenDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             fullscreen);

// ---- Frame capture (spec_version 5) ----

#define XR_WORKSPACE_CAPTURE_PATH_MAX_DXR 256

typedef XrFlags64 XrWorkspaceCaptureFlagsDXR;

// Bitmask of which view variants the runtime should write. ATLAS is the
// combined side-by-side back buffer the compositor presents to the display
// processor; future flags may select per-eye images, depth, etc.
static const XrWorkspaceCaptureFlagsDXR XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_DXR = 0x00000001u;

/*!
 * @brief Request struct for xrCaptureWorkspaceFrameDXR.
 *
 * The runtime appends format-specific suffixes (e.g. "_atlas.png") to
 * @c pathPrefix. The IPC schema only carries POD types so the prefix is
 * an in-struct char array rather than a separately-allocated string.
 */
typedef struct XrWorkspaceCaptureRequestDXR {
    XrStructureType            type;       // XR_TYPE_WORKSPACE_CAPTURE_REQUEST_DXR
    void* XR_MAY_ALIAS         next;
    char                       pathPrefix[XR_WORKSPACE_CAPTURE_PATH_MAX_DXR];
    XrWorkspaceCaptureFlagsDXR flags;
} XrWorkspaceCaptureRequestDXR;

/*!
 * @brief Result returned by xrCaptureWorkspaceFrameDXR.
 *
 * The metadata is intended to support a sidecar JSON describing the capture
 * (atlas/eye dimensions, stereo layout, display physical size, eye poses at
 * capture time).
 */
typedef struct XrWorkspaceCaptureResultDXR {
    XrStructureType            type;       // XR_TYPE_WORKSPACE_CAPTURE_RESULT_DXR
    void* XR_MAY_ALIAS         next;
    uint64_t                   timestampNs;
    uint32_t                   atlasWidth;
    uint32_t                   atlasHeight;
    uint32_t                   eyeWidth;
    uint32_t                   eyeHeight;
    XrWorkspaceCaptureFlagsDXR viewsWritten; // Subset of request flags actually written.
    uint32_t                   tileColumns;
    uint32_t                   tileRows;
    float                      displayWidthM;
    float                      displayHeightM;
    float                      eyeLeftM[3];
    float                      eyeRightM[3];
} XrWorkspaceCaptureResultDXR;

/*!
 * @brief Capture the current workspace composite frame to disk.
 *
 * The runtime reads the D3D11 composite back buffer at the next safe
 * moment, writes the requested view variants to files named after
 * @c request->pathPrefix, and fills @c result with metadata describing
 * the capture. Used by workspace controllers to implement screenshot /
 * recording features without giving them direct access to client
 * swapchains.
 *
 * @param session A valid workspace session.
 * @param request The capture request (path prefix + flags).
 * @param result  Output: capture metadata.
 */
typedef XrResult (XRAPI_PTR *PFN_xrCaptureWorkspaceFrameDXR)(
    XrSession                            session,
    const XrWorkspaceCaptureRequestDXR  *request,
    XrWorkspaceCaptureResultDXR         *result);

// ---- Client enumeration (spec_version 5) ----

/*!
 * @brief Per-client metadata returned by xrGetWorkspaceClientInfoDXR.
 *
 * Stable for the lifetime of the connection. Capture clients (adopted via
 * xrAddWorkspaceCaptureClientDXR) report their controller-supplied name;
 * OpenXR clients report the application_name they passed at xrCreateInstance.
 */
typedef struct XrWorkspaceClientInfoDXR {
    XrStructureType         type;       // XR_TYPE_WORKSPACE_CLIENT_INFO_DXR
    void* XR_MAY_ALIAS      next;
    XrWorkspaceClientId     clientId;
    XrWorkspaceClientTypeDXR clientType;
    char                    name[XR_MAX_APPLICATION_NAME_SIZE];
    uint64_t                pid;        // platform PID (DWORD on Windows, pid_t elsewhere)
    XrBool32                isFocused;
    XrBool32                isVisible;
    uint32_t                zOrder;
} XrWorkspaceClientInfoDXR;

/*!
 * @brief Enumerate workspace clients.
 *
 * Returns the OpenXR clients currently connected to the workspace. Capture
 * clients (adopted via xrAddWorkspaceCaptureClientDXR) are tracked
 * controller-side and are not returned here — the controller already knows
 * their ids since it added them.
 *
 * Standard two-call enumerate idiom: pass capacityInput=0 to get the count,
 * then pass an array sized to *countOutput on the second call.
 *
 * @param session       A valid workspace session.
 * @param capacityInput Capacity of @p clientIds; 0 for count-query.
 * @param countOutput   Output: number of clientIds available (or written).
 * @param clientIds     Output array; may be NULL when capacityInput == 0.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateWorkspaceClientsDXR)(
    XrSession            session,
    uint32_t             capacityInput,
    uint32_t            *countOutput,
    XrWorkspaceClientId *clientIds);

/*!
 * @brief Read metadata for a single workspace client.
 *
 * @param session   A valid workspace session.
 * @param clientId  The client to query.
 * @param info      Output: filled XrWorkspaceClientInfoDXR. Caller must set
 *                  info->type = XR_TYPE_WORKSPACE_CLIENT_INFO_DXR before the call.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceClientInfoDXR)(
    XrSession                  session,
    XrWorkspaceClientId        clientId,
    XrWorkspaceClientInfoDXR  *info);

// ---- Controller-owned chrome (spec_version 7) ----

/*!
 * @brief Maximum hit regions a controller can register per chrome layout.
 *
 * Fixed-size cap so the IPC wire form stays POD.
 */
#define XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_DXR 8

/*!
 * @brief Create-info for xrCreateWorkspaceClientChromeSwapchainDXR.
 *
 * Describes the 2D chrome image the controller will draw to. The runtime
 * mints a swapchain whose images are cross-process-shareable (D3D11
 * KEYEDMUTEX + NTHANDLE) so the controller's D3D11 device can author the
 * pixels in its own process. Format must be a 2D color format the runtime
 * supports for blits (today: DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).
 */
typedef struct XrWorkspaceChromeSwapchainCreateInfoDXR {
    XrStructureType       type;        // XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    int64_t               format;       // graphics-API native format (e.g. DXGI_FORMAT)
    uint32_t              width;
    uint32_t              height;
    uint32_t              sampleCount;  // 1 (no MSAA) for the pill chrome
    uint32_t              mipCount;     // 1
} XrWorkspaceChromeSwapchainCreateInfoDXR;

/*!
 * @brief One controller-defined hit region inside a chrome quad.
 *
 * The runtime ray-casts the chrome quad and looks up the first region whose
 * UV bounds contain the hit point. Region @c id is echoed back on POINTER /
 * POINTER_MOTION events as @c chromeRegionId — opaque to the runtime, the
 * controller decides what each id means (grip, close, minimize, …).
 */
typedef struct XrWorkspaceChromeHitRegionDXR {
    XrWorkspaceChromeRegionIdDXR id;       // controller-defined; 0 = no region
    XrRect2Df                    bounds;   // chrome-UV space [0,1]^2; offset = top-left, extent = size
} XrWorkspaceChromeHitRegionDXR;

/*!
 * @brief Layout for a controller-submitted chrome quad.
 *
 * The controller calls xrSetWorkspaceClientChromeLayoutDXR once on connect
 * and on every layout change (preset switch, resize). The runtime caches the
 * layout per client and applies it every render. Per-frame IPC is not needed.
 *
 * @c poseInClient is the chrome quad's pose relative to the client window's
 * own pose — typically {orient = identity, position = (0, +window_h/2 + gap +
 * pill_h/2, 0)} for the floating-pill design. @c sizeMeters is the quad's
 * physical extent.
 *
 * If @c followsWindowOrient is XR_TRUE the chrome rotates with the window
 * (useful for the carousel preset). XR_FALSE keeps it axis-aligned to the
 * display regardless of window orientation.
 *
 * @c depthBiasMeters biases the chrome quad toward the eye in NDC; default
 * 0.001 m matches the runtime's prior pill-over-content depth bias. 0 means
 * "use runtime default."
 */
typedef struct XrWorkspaceChromeLayoutDXR {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_CHROME_LAYOUT_DXR
    const void* XR_MAY_ALIAS next;
    XrPosef          poseInClient;
    XrExtent2Df      sizeMeters;
    XrBool32         followsWindowOrient;
    uint32_t         hitRegionCount;     // <= XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_DXR
    const XrWorkspaceChromeHitRegionDXR* hitRegions;
    float            depthBiasMeters;    // 0 = runtime default (0.001)
    // spec_version 8: auto-anchor flags so chrome stays attached to a window
    // edge as the window resizes WITHOUT the controller having to re-push the
    // layout on every WINDOW_POSE_CHANGED event. Without these the chrome
    // appears to lag one frame behind the window edge during a resize drag,
    // because the controller's push_layout always carries the win_h_m value
    // from the LAST frame the controller observed.
    //   anchorToWindowTopEdge: when XR_TRUE, poseInClient.position.y is
    //     interpreted as an offset ABOVE the window's top edge (positive =
    //     above) instead of from the window center. Runtime evaluates
    //     effectiveY = window_h/2 + poseInClient.position.y per frame using
    //     the CURRENT window height — chrome stays glued to top edge.
    //   widthAsFractionOfWindow: when > 0, chrome width is computed each
    //     frame as window_w * widthAsFractionOfWindow (sizeMeters.width is
    //     ignored). 0 = absolute (use sizeMeters.width).
    XrBool32         anchorToWindowTopEdge;
    float            widthAsFractionOfWindow;
} XrWorkspaceChromeLayoutDXR;

/*!
 * @brief Create a chrome swapchain for a workspace client.
 *
 * The runtime creates a cross-process-shareable image-loop swapchain
 * dedicated to chrome rendering for the given client. The swapchain returned
 * is a normal XrSwapchain — Acquire / Wait / Release operate as usual; the
 * runtime tracks the magic handle internally so it composites the released
 * image onto the workspace at the layout-specified pose.
 *
 * Most controllers create one chrome swapchain per client they decorate. The
 * shell may filter out itself (its own session) since the controller does
 * not decorate its own session.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace,
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the extension was not enabled.
 */
typedef XrResult (XRAPI_PTR *PFN_xrCreateWorkspaceClientChromeSwapchainDXR)(
    XrSession                                       session,
    XrWorkspaceClientId                             clientId,
    const XrWorkspaceChromeSwapchainCreateInfoDXR  *createInfo,
    XrSwapchain                                    *swapchain);

/*!
 * @brief Destroy a chrome swapchain.
 *
 * Equivalent to xrDestroySwapchain for a chrome swapchain — the runtime
 * stops compositing its image and releases the underlying texture. The
 * client's window stays visible (just without chrome) until a new chrome
 * swapchain is submitted.
 */
typedef XrResult (XRAPI_PTR *PFN_xrDestroyWorkspaceClientChromeSwapchainDXR)(
    XrSwapchain swapchain);

/*!
 * @brief Set / update the layout of a client's chrome quad.
 *
 * Layout is cached per client and re-applied every render. Call once on
 * connect after creating the chrome swapchain, and again whenever the
 * controller changes the quad's pose, size, hit regions, or depth bias.
 *
 * Calling this with a clientId that has no chrome swapchain stores the
 * layout for later — when the controller later creates a chrome swapchain
 * for that client, the cached layout takes effect.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_VALIDATION_FAILURE if hitRegionCount exceeds
 *         XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_DXR,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientChromeLayoutDXR)(
    XrSession                          session,
    XrWorkspaceClientId                clientId,
    const XrWorkspaceChromeLayoutDXR  *layout);

/*!
 * @brief Per-frame pose update for a client's chrome quad (spec_version 12).
 *
 * @c xrSetWorkspaceClientChromeLayoutDXR is the cached-layout fast path: call
 * it once on connect and whenever the chrome's *size*, hit regions, depth bias,
 * or anchor flags change. It is documented as "per-frame IPC is not needed."
 *
 * This call is the pose-only complement, intended for fast-moving chrome: a
 * cursor sprite whose Z depth tracks the per-frame hit-test, a focus-ring
 * animation, a tooltip that follows the user's gaze, etc. Carries only the
 * 3-vec + quaternion (~32 bytes on the wire) and overwrites the @c poseInClient
 * field of the previously-cached layout. All other fields stay as last set.
 *
 * If no layout has been set for @p clientId yet, the call still records the
 * pose; once the controller pushes a layout, the recorded pose is used as the
 * starting value.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrUpdateWorkspaceClientChromeLayerPoseDXR)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    const XrPosef       *poseInClient);

// ---- Workspace cursor (spec_version 13) ----
//
// The runtime owns the per-tile per-eye cursor render path (hit-Z disparity,
// over-window dimming, multi-view composition). The CONTROLLER owns the
// sprite content (any cursor look the controller wants — animated, branded,
// shape-by-state, etc.). The split: runtime is mechanism, controller is
// content.
//
// Flow:
//   1. Controller creates a session-global cursor swapchain via
//      xrCreateWorkspaceCursorSwapchainDXR. This is a single-image color
//      swapchain (acquire / wait / fill / release) like chrome swapchains
//      but NOT bound to any specific client — it's owned by the session.
//   2. Controller renders its sprite into the swapchain image once (or per
//      frame for animation).
//   3. Controller calls xrSetWorkspaceCursorDXR with the swapchain handle,
//      hot spot (in sprite UV), physical size, and visibility flag. The
//      runtime samples the swapchain texture in its existing per-tile
//      cursor render pass.
//   4. To hide the cursor without destroying the swapchain: call
//      xrSetWorkspaceCursorDXR with visible=XR_FALSE (or swapchain=XR_NULL_HANDLE).
//   5. To change cursor shape: render a new sprite into the swapchain
//      (acquire/wait/UpdateSubresource/release). The runtime samples the
//      latest released image automatically. Same swapchain — no re-set.
//
// Architectural memory: [[chrome-layers-not-for-globals]] — chrome layers
// are per-client and tile-scissored in shell-mode atlases; not suitable
// for cross-tile globals like cursor. This RPC pair is the right shape.

/*!
 * @brief Create-info for xrCreateWorkspaceCursorSwapchainDXR.
 *
 * Same shape as chrome swapchain create-info but the resulting swapchain is
 * session-global, not bound to any workspace client.
 */
typedef struct XrWorkspaceCursorSwapchainCreateInfoDXR {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_CURSOR_SWAPCHAIN_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    int64_t          format;      // Graphics-API format (DXGI_FORMAT, VK_FORMAT, etc.)
    uint32_t         width;
    uint32_t         height;
    uint32_t         sampleCount; // 1 if 0
    uint32_t         mipCount;    // 1 if 0
} XrWorkspaceCursorSwapchainCreateInfoDXR;

/*!
 * @brief Create a session-global swapchain to source the cursor sprite from.
 *
 * The returned swapchain behaves like any other OpenXR swapchain: caller
 * acquires images, fills them (CPU upload or render-target draw), and
 * releases. The runtime composes the latest-released image as the cursor
 * sprite, scaled to @ref XrWorkspaceCursorInfoDXR::sizeMeters with the
 * specified hot spot.
 *
 * Sample format choices: DXGI_FORMAT_R8G8B8A8_UNORM (linear alpha-blend),
 * DXGI_FORMAT_R8G8B8A8_UNORM_SRGB (gamma alpha-blend). The runtime sample
 * path uses linear blending; pre-multiplied alpha sprites blend correctly
 * in either format.
 *
 * @return XR_SUCCESS, XR_ERROR_VALIDATION_FAILURE (bad createInfo),
 *         XR_ERROR_FEATURE_UNSUPPORTED (session not in workspace IPC mode).
 */
typedef XrResult (XRAPI_PTR *PFN_xrCreateWorkspaceCursorSwapchainDXR)(
    XrSession                                        session,
    const XrWorkspaceCursorSwapchainCreateInfoDXR  *createInfo,
    XrSwapchain                                    *swapchain);

/*!
 * @brief Active cursor parameters.
 *
 * Pushed via xrSetWorkspaceCursorDXR. The runtime composes the sprite at the
 * OS cursor position with per-eye disparity derived from the depth the
 * controller pushes each frame via xrSetWorkspaceCursorDepthDXR.
 */
typedef struct XrWorkspaceCursorInfoDXR {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_CURSOR_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrSwapchain      swapchain;   // Source for cursor pixels; XR_NULL_HANDLE = hide
    XrVector2f       hotSpot;     // Normalized UV [0,1] of the sprite's click point
    float            sizeMeters;  // Physical size (width = height) in meters
    XrBool32         visible;     // XR_FALSE hides without releasing the swapchain
} XrWorkspaceCursorInfoDXR;

/*!
 * @brief Activate / update the workspace cursor.
 *
 * Call once on startup after creating the cursor swapchain and filling its
 * first image. Call again whenever the controller wants to change hot spot,
 * size, visibility, or swap to a different swapchain (e.g., for a different
 * cursor shape). The runtime keeps the most recent info; per-frame rendering
 * uses the cached state, so this is a low-frequency call (NOT per-frame).
 *
 * Setting @p info->visible to XR_FALSE OR @p info->swapchain to
 * XR_NULL_HANDLE hides the cursor without tearing down anything; a
 * subsequent call re-activates.
 *
 * @return XR_SUCCESS, XR_ERROR_VALIDATION_FAILURE (bad info type / size),
 *         XR_ERROR_HANDLE_INVALID (swapchain not a workspace-mode swapchain
 *         or already destroyed), XR_ERROR_FEATURE_UNSUPPORTED (session not
 *         in workspace IPC mode).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceCursorDXR)(
    XrSession                       session,
    const XrWorkspaceCursorInfoDXR *info);

// ---- Cursor depth (spec_version 22; dimFactor added in spec_version 23) ----

/*!
 * @brief Per-frame cursor depth, supplied by the workspace controller.
 *
 * In spec_version 22 the runtime no longer raycasts to find the depth of the
 * window under the cursor. The controller — which owns window placement and
 * already runs its own eye→cursor hit-test — pushes the resulting depth here
 * each frame. The runtime feeds @ref hitZMeters into its existing per-eye
 * disparity math so the cursor sprite floats at the depth of what it points
 * at, and uses @ref overWindow to dim the cursor over windows (crosstalk
 * mitigation). When a modal grab pins the cursor to the zero-disparity plane,
 * the controller pushes hitZMeters = 0.
 *
 * spec_version 23 adds @ref dimFactor: the controller — which owns cursor
 * look-and-feel — pushes the over-window cursor body alpha rather than the
 * runtime hardcoding it. Only applied when @ref overWindow is XR_TRUE; 1.0 =
 * no dim, lower = more transparent (the previous runtime default was 0.30).
 */
typedef struct XrWorkspaceCursorDepthDXR {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_CURSOR_DEPTH_DXR
    const void* XR_MAY_ALIAS next;
    float            hitZMeters;  // Ray-plane depth under cursor (meters); 0 = panel plane
    XrBool32         overWindow;  // XR_TRUE when the cursor is over a window
    float            dimFactor;   // (spec 23) over-window cursor body alpha [0,1]; 1 = no dim
} XrWorkspaceCursorDepthDXR;

/*!
 * @brief Push the current cursor depth to the runtime.
 *
 * Call once per frame while the workspace cursor is visible (this IS a
 * per-frame call, unlike xrSetWorkspaceCursorDXR). The runtime caches the
 * most recent value and applies it to the next composited frame; cursor
 * screen position itself is still owned by the runtime (OS cursor).
 *
 * @return XR_SUCCESS, XR_ERROR_VALIDATION_FAILURE (bad info type),
 *         XR_ERROR_FEATURE_UNSUPPORTED (session not in workspace IPC mode).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceCursorDepthDXR)(
    XrSession                        session,
    const XrWorkspaceCursorDepthDXR *info);

// ---- Workspace overlay (spec_version 17) ----
//
// A workspace overlay is a controller-pushed, non-client swapchain the runtime
// composites at a controller-chosen docked position on the zero-disparity plane
// (z = 0). It is the generic mechanism behind controller-owned display-spanning
// UI such as a taskbar: the controller renders the UI into the swapchain and
// tells the runtime where to dock it (normalized display anchor + sprite pivot)
// and how big it is in physical meters. Unlike the cursor, the overlay has no
// raycast depth and no per-eye disparity — at z = 0 the same sprite is drawn at
// the same docked position in every atlas tile. Single global overlay slot per
// session (mirrors the cursor); a subsequent set replaces the prior overlay.
//
// Architectural memory: [[chrome-layers-not-for-globals]] — chrome layers are
// per-client and tile-scissored; not suitable for cross-tile globals. This RPC
// pair (like the cursor's) is the right shape for display-spanning UI.

/*!
 * @brief Create-info for xrCreateWorkspaceOverlaySwapchainDXR.
 *
 * Same shape as the cursor swapchain create-info: the resulting swapchain is
 * session-global, not bound to any workspace client.
 */
typedef struct XrWorkspaceOverlaySwapchainCreateInfoDXR {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_OVERLAY_SWAPCHAIN_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    int64_t          format;      // Graphics-API format (DXGI_FORMAT, VK_FORMAT, etc.)
    uint32_t         width;
    uint32_t         height;
    uint32_t         sampleCount; // 1 if 0
    uint32_t         mipCount;    // 1 if 0
} XrWorkspaceOverlaySwapchainCreateInfoDXR;

/*!
 * @brief Create a session-global swapchain to source overlay pixels from.
 *
 * Behaves like any other OpenXR swapchain: caller acquires images, fills them
 * (CPU upload or render-target draw), and releases. The runtime composes the
 * latest-released image as the overlay, scaled to @ref
 * XrWorkspaceOverlayInfoDXR::sizeMeters and docked per its anchor/pivot.
 *
 * @return XR_SUCCESS, XR_ERROR_VALIDATION_FAILURE (bad createInfo),
 *         XR_ERROR_FEATURE_UNSUPPORTED (session not in workspace IPC mode).
 */
typedef XrResult (XRAPI_PTR *PFN_xrCreateWorkspaceOverlaySwapchainDXR)(
    XrSession                                         session,
    const XrWorkspaceOverlaySwapchainCreateInfoDXR  *createInfo,
    XrSwapchain                                     *swapchain);

/*!
 * @brief Active overlay parameters.
 *
 * Pushed via xrSetWorkspaceOverlayDXR. The overlay is docked at z = 0 (zero
 * disparity), so there is no per-eye disparity or raycast depth: @p anchor is a
 * normalized position in display space ([0,1], origin top-left; e.g. {0.5,1.0}
 * is bottom-center) and @p pivot is the normalized UV within the overlay sprite
 * ([0,1]; e.g. {0.5,1.0} is the sprite's bottom-center) that lands on @p anchor.
 * @p sizeMeters is the physical width/height of the overlay on the display.
 */
typedef struct XrWorkspaceOverlayInfoDXR {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_OVERLAY_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrSwapchain      swapchain;   // Source for overlay pixels; XR_NULL_HANDLE = hide
    XrVector2f       anchor;      // Normalized display position [0,1] of the dock point
    XrVector2f       pivot;       // Normalized sprite UV [0,1] mapped onto anchor
    XrExtent2Df      sizeMeters;  // Physical width,height in meters
    XrBool32         visible;     // XR_FALSE hides without releasing the swapchain
    // spec_version 19: side-by-side stereo overlay. When XR_TRUE the swapchain
    // image is treated as two halves — left half = left-eye content, right half
    // = right-eye content — and the runtime samples the matching half per eye
    // view (stretched to the full overlay footprint). The controller renders the
    // SAME z=0 content into both halves except for elements it wants to appear
    // stereoscopic (e.g. 3D app-icon SBS images), which differ between halves.
    // XR_FALSE (default) = mono: the whole image is composited identically in
    // every eye (zero disparity).
    XrBool32         stereoSideBySide;
    // spec_version 21: keyed multi-overlay support. The runtime keeps a small
    // map of overlays keyed by overlayId and composites ALL live ids each frame
    // in ascending-id z-order (lower id behind, higher id in front), so the
    // taskbar, launcher, and a toast can show simultaneously. Each call targets
    // exactly the slot named by overlayId: it creates the slot if new, replaces
    // its parameters if it already exists, and — when visible is XR_FALSE OR
    // swapchain is XR_NULL_HANDLE — removes that id from the map (the other
    // overlays are untouched). overlayId 0 is the default slot: a controller
    // that never sets the field sees exactly the pre-v21 single-overlay
    // behavior (one slot, overwritten on every call).
    uint32_t         overlayId;
} XrWorkspaceOverlayInfoDXR;

/*!
 * @brief Activate / update a workspace overlay.
 *
 * Call once on startup after creating the overlay swapchain and filling its
 * first image, then again whenever the controller wants to change anchor,
 * pivot, size, visibility, or swap to a different swapchain. The runtime keeps
 * the most recent info; per-frame rendering uses the cached state, so this is a
 * low-frequency call (NOT per-frame).
 *
 * spec_version 21: each call targets the slot named by @p info->overlayId. The
 * runtime keeps a small map of overlays keyed by id and composites all live ids
 * each frame in ascending-id z-order, so multiple overlays (taskbar, launcher,
 * toast, ...) coexist. Use distinct ids for overlays that must show at once;
 * reuse an id to replace that overlay's parameters. overlayId 0 is the default
 * slot, so a controller that never sets the field keeps the pre-v21 behavior of
 * a single overlay overwritten on every call.
 *
 * Setting @p info->visible to XR_FALSE OR @p info->swapchain to XR_NULL_HANDLE
 * removes the overlay named by @p info->overlayId without tearing down anything
 * (other ids are untouched); a subsequent call re-activates that id.
 *
 * @return XR_SUCCESS, XR_ERROR_VALIDATION_FAILURE (bad info type / size),
 *         XR_ERROR_HANDLE_INVALID (swapchain not a workspace-mode swapchain or
 *         already destroyed), XR_ERROR_FEATURE_UNSUPPORTED (session not in
 *         workspace IPC mode).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceOverlayDXR)(
    XrSession                        session,
    const XrWorkspaceOverlayInfoDXR *info);

// ---- Modal input grab (spec_version 18) ----

/*!
 * @brief Grab / release all user input for the workspace controller.
 *
 * While grabbed (@p grab == XR_TRUE) the runtime stops forwarding keyboard,
 * mouse-button, and scroll-wheel input to the focused workspace client and
 * routes every event to the controller through the public input-event ring
 * (xrEnumerateWorkspaceInputEventsDXR): KEY, POINTER, and SCROLL events are
 * delivered regardless of which client is focused or whether the cursor is
 * over app content. This is the mechanism a controller uses to drive a modal
 * UI it composites onto a workspace overlay (e.g. an app launcher band): the
 * controller owns the input while its UI is up and releases the grab when the
 * UI dismisses, restoring normal app forwarding.
 *
 * The grab is session-global (mirrors the cursor / overlay slots) and does NOT
 * change focus — the previously focused client stays focused and simply stops
 * receiving forwarded input until the grab is released. Idempotent: setting the
 * same state twice is a no-op.
 *
 * @param session A valid workspace session (controller side).
 * @param grab    XR_TRUE to grab input for the controller; XR_FALSE to release.
 *
 * @return XR_SUCCESS, XR_ERROR_FEATURE_UNSUPPORTED (session not in workspace
 *         IPC mode).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceInputGrabDXR)(
    XrSession session,
    XrBool32  grab);

// ---- Event-driven wakeup (spec_version 8) ----

/*!
 * @brief Acquire a wakeup event the controller can wait on instead of polling.
 *
 * Returns a platform-native event handle that the runtime signals whenever
 * async workspace state changes (input event pushed onto the drain queue,
 * focused-slot transition, hovered-slot transition). The controller passes
 * the handle to a wait primitive (Win32 WaitForSingleObject /
 * MsgWaitForMultipleObjects, or analogous on other platforms) so its main
 * loop sleeps when nothing is happening and wakes immediately when there
 * is something to drain.
 *
 * Auto-reset semantics: the handle becomes signaled on each runtime
 * SetEvent and is cleared automatically when one waiter wakes. Multiple
 * SetEvent calls in quick succession may collapse to a single wake — the
 * controller is expected to drain ALL pending state on each wake (call
 * xrEnumerateWorkspaceInputEventsDXR, re-query whatever it cares about).
 *
 * The handle is a Win32 HANDLE on Windows, returned as @p outNativeHandle
 * cast to uint64_t. Caller takes ownership and must close (CloseHandle)
 * when done. Calling this function multiple times returns fresh handles
 * each time; the runtime keeps a single source-of-truth event internally
 * and duplicates it to the caller's process at each call.
 *
 * Platforms other than Windows currently return XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * Idle CPU cost goes from ~0.1% of one core (the polling baseline) to
 * effectively 0 when this handle is wired into the controller's wait set.
 *
 * @param session            A valid workspace session.
 * @param outNativeHandle    Output: platform-native event handle as uint64_t.
 *                           NULL on error.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAcquireWorkspaceWakeupEventDXR)(
    XrSession  session,
    uint64_t  *outNativeHandle);

// ---- Per-client visual style (spec_version 9) ----

/*!
 * @brief Per-client visual style applied at workspace content blit time.
 *
 * The runtime is a parameterized composite engine; this struct is the
 * controller's surface for adjusting the existing shader knobs (corner
 * radius, edge feather, focus glow). The runtime owns mechanism (sampling
 * the client's swapchain, applying the alpha mask, compositing onto the
 * atlas); the controller owns appearance by pushing values into this
 * struct. New visual treatments add new fields here additively over time
 * — controllers that don't know about a new field see it default to a
 * runtime-defined sane value.
 *
 * Coordinate system: @c edgeFeatherMeters and @c focusGlowFalloffMeters
 * are measured in physical display space — independent of window size.
 * The same 3 mm feather reads identically on a small or large window.
 *
 * Set per-client via xrSetWorkspaceClientStyleDXR. Runtime caches the
 * style per client and applies every render. Pass a zero-initialized
 * style (or NULL) to reset to runtime defaults.
 */
typedef struct XrWorkspaceClientStyleDXR {
    XrStructureType       type;        // XR_TYPE_WORKSPACE_CLIENT_STYLE_DXR
    const void* XR_MAY_ALIAS next;

    // Edge geometry. cornerRadius is a fraction of window HEIGHT (0..1).
    // 0 = sharp corners. Typical values 0.02..0.08 read as soft / deliberate
    // rounding. Always rounds all four corners. Negative values reserved
    // for future "top-only" / "bottom-only" semantics; treat as 0 today.
    float cornerRadius;

    // Soft alpha falloff at the perimeter (rounded square + edges) in
    // METERS. 0 = crisp pixel-perfect edge. Positive values fade the
    // alpha toward 0 over this physical width — reads as a subtle
    // ambient softening on every window. Always applied (focused or
    // not). Typical values 0.001..0.004 m (1-4 mm).
    float edgeFeatherMeters;

    // Focus glow. Active only when this client is the focused workspace
    // client (per xrSetWorkspaceFocusedClientDXR). Ignored when the
    // client is not focused — no effect on unfocused windows.
    //
    //   focusGlowColor       RGBA. Alpha is the peak opacity at the
    //                        inner edge of the glow band. Premultiplied
    //                        is fine; the runtime composites linearly.
    //   focusGlowIntensity   Scalar multiplier on color.a. 0 disables
    //                        the glow even when focused; ~0.6..1.0
    //                        reads as a clear focus indicator.
    //   focusGlowFalloffMeters
    //                        Distance from the rounded-square perimeter
    //                        where the glow's alpha falls off. ~5-15
    //                        mm reads as a soft halo. 0 disables.
    float focusGlowColor[4];
    float focusGlowIntensity;
    float focusGlowFalloffMeters;
} XrWorkspaceClientStyleDXR;

/*!
 * @brief Set / update the per-client visual style.
 *
 * Cached per client; applied every render. Call once on connect after
 * adding the client to the workspace, and again whenever the controller
 * changes the visual treatment (preset switch, theme change, focus
 * indicator scheme). Calling with a clientId that has no live slot
 * stores the style for later — when the slot binds, the cached style
 * takes effect.
 *
 * Pass @p style = NULL to reset that client's style to runtime defaults.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_VALIDATION_FAILURE if any numeric field is non-finite
 *         or negative where required (cornerRadius, edgeFeatherMeters,
 *         focusGlowIntensity, focusGlowFalloffMeters all >= 0),
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active
 *         workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientStyleDXR)(
    XrSession                              session,
    XrWorkspaceClientId                    clientId,
    const XrWorkspaceClientStyleDXR       *style);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrActivateSpatialWorkspaceDXR(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrDeactivateSpatialWorkspaceDXR(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrGetSpatialWorkspaceStateDXR(
    XrSession session,
    XrBool32 *out_active);

XRAPI_ATTR XrResult XRAPI_CALL xrAddWorkspaceCaptureClientDXR(
    XrSession            session,
    uint64_t             nativeWindow,
    const char          *nameOptional,
    XrWorkspaceClientId *outClientId);

XRAPI_ATTR XrResult XRAPI_CALL xrRemoveWorkspaceCaptureClientDXR(
    XrSession           session,
    XrWorkspaceClientId clientId);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientWindowPoseDXR(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    const XrPosef       *pose,
    float                widthMeters,
    float                heightMeters);

XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceClientWindowPoseDXR(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrPosef             *outPose,
    float               *outWidthMeters,
    float               *outHeightMeters);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientVisibilityDXR(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             visible);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceFocusedClientDXR(
    XrSession           session,
    XrWorkspaceClientId clientId);

XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceFocusedClientDXR(
    XrSession            session,
    XrWorkspaceClientId *outClientId);

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateWorkspaceInputEventsDXR(
    XrSession                  session,
    uint32_t                   capacityInput,
    uint32_t                  *countOutput,
    XrWorkspaceInputEventDXR  *events);

XRAPI_ATTR XrResult XRAPI_CALL xrEnableWorkspacePointerCaptureDXR(
    XrSession session,
    uint32_t  button);

XRAPI_ATTR XrResult XRAPI_CALL xrDisableWorkspacePointerCaptureDXR(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrRequestWorkspaceClientExitDXR(
    XrSession            session,
    XrWorkspaceClientId  clientId);

XRAPI_ATTR XrResult XRAPI_CALL xrRequestWorkspaceClientFullscreenDXR(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             fullscreen);

XRAPI_ATTR XrResult XRAPI_CALL xrCaptureWorkspaceFrameDXR(
    XrSession                            session,
    const XrWorkspaceCaptureRequestDXR  *request,
    XrWorkspaceCaptureResultDXR         *result);

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateWorkspaceClientsDXR(
    XrSession            session,
    uint32_t             capacityInput,
    uint32_t            *countOutput,
    XrWorkspaceClientId *clientIds);

XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceClientInfoDXR(
    XrSession                  session,
    XrWorkspaceClientId        clientId,
    XrWorkspaceClientInfoDXR  *info);

XRAPI_ATTR XrResult XRAPI_CALL xrCreateWorkspaceClientChromeSwapchainDXR(
    XrSession                                       session,
    XrWorkspaceClientId                             clientId,
    const XrWorkspaceChromeSwapchainCreateInfoDXR  *createInfo,
    XrSwapchain                                    *swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyWorkspaceClientChromeSwapchainDXR(
    XrSwapchain swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientChromeLayoutDXR(
    XrSession                          session,
    XrWorkspaceClientId                clientId,
    const XrWorkspaceChromeLayoutDXR  *layout);

XRAPI_ATTR XrResult XRAPI_CALL xrUpdateWorkspaceClientChromeLayerPoseDXR(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    const XrPosef       *poseInClient);

XRAPI_ATTR XrResult XRAPI_CALL xrCreateWorkspaceCursorSwapchainDXR(
    XrSession                                        session,
    const XrWorkspaceCursorSwapchainCreateInfoDXR  *createInfo,
    XrSwapchain                                    *swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceCursorDXR(
    XrSession                       session,
    const XrWorkspaceCursorInfoDXR *info);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceCursorDepthDXR(
    XrSession                        session,
    const XrWorkspaceCursorDepthDXR *info);

XRAPI_ATTR XrResult XRAPI_CALL xrCreateWorkspaceOverlaySwapchainDXR(
    XrSession                                         session,
    const XrWorkspaceOverlaySwapchainCreateInfoDXR  *createInfo,
    XrSwapchain                                     *swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceOverlayDXR(
    XrSession                        session,
    const XrWorkspaceOverlayInfoDXR *info);

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireWorkspaceWakeupEventDXR(
    XrSession  session,
    uint64_t  *outNativeHandle);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientStyleDXR(
    XrSession                              session,
    XrWorkspaceClientId                    clientId,
    const XrWorkspaceClientStyleDXR       *style);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceInputGrabDXR(
    XrSession session,
    XrBool32  grab);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_SPATIAL_WORKSPACE_H
