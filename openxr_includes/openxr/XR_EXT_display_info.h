// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Header for XR_EXT_display_info extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension exposes physical display properties and recommended view
 * scaling factors to the application via xrGetSystemProperties. The app
 * multiplies its current window size by the scale factors to compute the
 * per-eye render resolution each frame, eliminating the need for runtime
 * events on window resize.
 */
#ifndef XR_EXT_DISPLAY_INFO_H
#define XR_EXT_DISPLAY_INFO_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_display_info 1
#define XR_EXT_display_info_SPEC_VERSION 14
#define XR_EXT_DISPLAY_INFO_EXTENSION_NAME "XR_EXT_display_info"

// Reuse the type value from the deleted XR_EXT_dynamic_render_resolution
#define XR_TYPE_DISPLAY_INFO_EXT ((XrStructureType)1000999003)

/*!
 * @brief Display information returned by xrGetSystemProperties.
 *
 * When chained to XrSystemProperties via the next pointer, the runtime fills
 * in the physical display dimensions and recommended view scale factors.
 *
 * The application computes per-eye render resolution as:
 *   renderWidth  = (uint32_t)(windowWidth  * recommendedViewScaleX)
 *   renderHeight = (uint32_t)(windowHeight * recommendedViewScaleY)
 *
 * The scale factors are static display properties (sr_recommended / display_pixels)
 * that do not change with window resize.
 *
 * @extends XrSystemProperties
 */
typedef struct XrDisplayInfoEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_DISPLAY_INFO_EXT
    void* XR_MAY_ALIAS          next;       //!< Pointer to next structure in chain
    XrExtent2Df                 displaySizeMeters;          //!< Physical display size in meters
    XrVector3f                  nominalViewerPositionInDisplaySpace; //!< Nominal viewer position in display space (meters)
    float                       recommendedViewScaleX;      //!< Horizontal scale: sr_recommended_w / display_pixel_w
    float                       recommendedViewScaleY;      //!< Vertical scale: sr_recommended_h / display_pixel_h
    uint32_t                    displayPixelWidth;          //!< Native display panel width in pixels (0 if unknown)
    uint32_t                    displayPixelHeight;         //!< Native display panel height in pixels (0 if unknown)
} XrDisplayInfoEXT;

/*!
 * @brief Display mode for XR_EXT_display_info 2D/3D switching.
 *
 * @deprecated Use xrRequestDisplayRenderingModeEXT instead. Each rendering mode
 * carries a hardwareDisplay3D field that triggers physical switching automatically.
 */
typedef enum XrDisplayModeEXT {
    XR_DISPLAY_MODE_2D_EXT = 0,
    XR_DISPLAY_MODE_3D_EXT = 1,
    XR_DISPLAY_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrDisplayModeEXT;

/*!
 * @brief Request display mode switch (2D/3D).
 *
 * @deprecated Use xrRequestDisplayRenderingModeEXT instead. This function is a
 * thin wrapper that finds the first rendering mode matching the requested
 * hardware display state and delegates to xrRequestDisplayRenderingModeEXT.
 *
 * Switches the display between 2D and 3D modes. In 3D mode, the display's
 * light field hardware is active for stereoscopic viewing. In 2D mode, the
 * display behaves as a conventional 2D panel.
 *
 * The runtime automatically switches to 3D mode on xrBeginSession and back
 * to 2D mode on xrEndSession.
 *
 * @param session A valid XrSession handle.
 * @param displayMode The desired display mode (2D or 3D).
 * @return XR_SUCCESS on success.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestDisplayModeEXT)(XrSession session, XrDisplayModeEXT displayMode);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestDisplayModeEXT(
    XrSession                                   session,
    XrDisplayModeEXT                            displayMode);
#endif

// ---- v6: Eye Tracking Mode Control ----

#define XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT ((XrStructureType)1000999006)
#define XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT        ((XrStructureType)1000999007)

/*!
 * @brief Eye tracking mode enum.
 *
 * MANAGED (0) is the default — apps that never call xrRequestEyeTrackingModeEXT
 * get current behavior (vendor SDK handles grace period + transitions).
 * MANUAL (1) provides unfiltered positions + explicit isTracking flag.
 */
typedef enum XrEyeTrackingModeEXT {
    XR_EYE_TRACKING_MODE_MANAGED_EXT  = 0,
    XR_EYE_TRACKING_MODE_MANUAL_EXT   = 1,
    XR_EYE_TRACKING_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrEyeTrackingModeEXT;

/*!
 * @brief Capability flags for eye tracking modes (bitmask).
 *
 * A value of 0 means the display has NO eye tracking capability at all.
 */
typedef XrFlags64 XrEyeTrackingModeCapabilityFlagsEXT;
static const XrEyeTrackingModeCapabilityFlagsEXT
    XR_EYE_TRACKING_MODE_CAPABILITY_NONE_EXT       = 0;
static const XrEyeTrackingModeCapabilityFlagsEXT
    XR_EYE_TRACKING_MODE_CAPABILITY_MANAGED_BIT_EXT = 0x00000001;
static const XrEyeTrackingModeCapabilityFlagsEXT
    XR_EYE_TRACKING_MODE_CAPABILITY_MANUAL_BIT_EXT = 0x00000002;

/*!
 * @brief Eye tracking mode capabilities — chained to XrSystemProperties.
 *
 * If supportedModes is 0 (NONE), the display has no eye tracking. In that
 * case defaultMode is undefined, xrRequestEyeTrackingModeEXT returns
 * XR_ERROR_FEATURE_UNSUPPORTED for any mode, and XrViewEyeTrackingStateEXT
 * always reports isTracking=XR_FALSE.
 *
 * xrLocateViews ALWAYS returns fully populated views (count, positions, FOVs)
 * regardless of tracking capability or state. The vendor SDK decides the view
 * positions (e.g., nominal viewer, last known, filtered). isTracking only
 * indicates whether those positions come from live eye tracking or a fallback.
 *
 * @extends XrSystemProperties
 */
typedef struct XrEyeTrackingModeCapabilitiesEXT {
    XrStructureType                        type;           //!< Must be XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT
    void* XR_MAY_ALIAS                     next;
    XrEyeTrackingModeCapabilityFlagsEXT    supportedModes; //!< Bitmask of supported modes (0 = no tracking)
    XrEyeTrackingModeEXT                   defaultMode;    //!< Mode used if app never requests one
} XrEyeTrackingModeCapabilitiesEXT;

/*!
 * @brief Per-frame eye tracking state — chained to XrViewState in xrLocateViews.
 *
 * xrLocateViews ALWAYS returns fully populated views (positions, FOVs)
 * regardless of isTracking. When isTracking is XR_FALSE, positions are
 * still valid — the vendor SDK populates them as it sees fit (e.g., last
 * known, nominal viewer, filtered). isTracking tells the app whether
 * positions come from live eye tracking or vendor-chosen fallback.
 * The app may use isTracking to trigger its own animations or UI.
 *
 * @extends XrViewState
 */
typedef struct XrViewEyeTrackingStateEXT {
    XrStructureType           type;       //!< Must be XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT
    void* XR_MAY_ALIAS        next;
    XrBool32                  isTracking; //!< XR_TRUE if eyes are actively tracked this frame
    XrEyeTrackingModeEXT     activeMode; //!< Currently active mode
} XrViewEyeTrackingStateEXT;

/*!
 * @brief Request eye tracking mode switch.
 *
 * Switches between managed and manual eye tracking modes. In managed mode,
 * the vendor SDK handles grace period + transitions internally. In manual mode,
 * the SDK provides unfiltered positions and the app uses isTracking to
 * handle tracking loss.
 *
 * @param session A valid XrSession handle.
 * @param mode The desired eye tracking mode.
 * @return XR_SUCCESS on success,
 *         XR_ERROR_FEATURE_UNSUPPORTED if the mode is not supported,
 *         XR_ERROR_VALIDATION_FAILURE if mode is invalid.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestEyeTrackingModeEXT)(
    XrSession session, XrEyeTrackingModeEXT mode);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestEyeTrackingModeEXT(
    XrSession               session,
    XrEyeTrackingModeEXT    mode);
#endif

// ---- v7: Display Rendering Mode Control ----

/*!
 * @brief Request a vendor-specific display rendering mode.
 *
 * Different 3D display vendors support multiple rendering variations
 * (e.g., side-by-side stereo, anaglyph, lenticular). This function lets
 * the application switch between them at runtime.
 *
 * Mode indices are vendor-defined:
 *   - Mode 0 = standard rendering (always available)
 *   - Mode 1+ = vendor-specific variations
 *
 * The runtime dispatches the request to the active display device, which
 * may accept or silently ignore unsupported indices.
 *
 * @param session A valid XrSession handle.
 * @param modeIndex The vendor-defined rendering mode index.
 * @return XR_SUCCESS on success.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestDisplayRenderingModeEXT)(
    XrSession session, uint32_t modeIndex);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestDisplayRenderingModeEXT(
    XrSession               session,
    uint32_t                modeIndex);
#endif

// ---- v8: Rendering Mode Enumeration ----

#define XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT ((XrStructureType)1000999008)

/*!
 * @brief Information about a single display rendering mode.
 *
 * Returned by xrEnumerateDisplayRenderingModesEXT to describe each available
 * vendor-specific rendering mode (e.g., side-by-side, anaglyph, lenticular).
 */
typedef struct XrDisplayRenderingModeInfoEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT
    void* XR_MAY_ALIAS          next;       //!< Pointer to next structure in chain
    uint32_t                    modeIndex;  //!< Vendor-defined mode index (pass to xrRequestDisplayRenderingModeEXT)
    char                        modeName[XR_MAX_SYSTEM_NAME_SIZE]; //!< Human-readable mode name
    uint32_t                    viewCount;  //!< Number of views (1=mono, 2=stereo, etc.)
    float                       viewScaleX; //!< Per-view horizontal scale (vendor-provided)
    float                       viewScaleY; //!< Per-view vertical scale (vendor-provided)
    XrBool32                    hardwareDisplay3D;  //!< Whether display hardware is in 3D mode
    uint32_t                    tileColumns;     //!< Tile columns in atlas layout (v12)
    uint32_t                    tileRows;        //!< Tile rows in atlas layout (v12)
    uint32_t                    viewWidthPixels; //!< Per-view width in pixels (v12)
    uint32_t                    viewHeightPixels;//!< Per-view height in pixels (v12)
    /*!
     * (v13) True for the mode that is currently active for this session.
     *
     * Apps can read this at startup (after xrCreateSession + first
     * xrEnumerateDisplayRenderingModesEXT call) to learn the current mode
     * without waiting for an XrEventDataRenderingModeChangedEXT — useful
     * when the session begins under a workspace that already chose a mode.
     * Re-enumerating after a mode change reflects the new active mode.
     */
    XrBool32                    isActive;
    /*!
     * (v13) True iff this session may request this mode via
     * xrRequestDisplayRenderingModeEXT.
     *
     * False for non-controller sessions running under a workspace — the
     * workspace controller is the sole mode authority and app requests are
     * dropped by the runtime. Apps can use this to gate their UI (e.g.
     * disable the V toggle and show "Mode locked by workspace"). Always
     * true for standalone sessions and for workspace-controller sessions.
     */
    XrBool32                    isRequestable;
} XrDisplayRenderingModeInfoEXT;

/*!
 * @brief Enumerate available display rendering modes.
 *
 * Standard OpenXR two-call enumerate pattern. First call with
 * modeCapacityInput=0 to query modeCountOutput, then allocate and call again.
 *
 * @param session              A valid XrSession handle.
 * @param modeCapacityInput    Capacity of the modes array (0 to query count).
 * @param modeCountOutput      Output: number of modes available.
 * @param modes                Output array of mode info structs.
 * @return XR_SUCCESS on success, XR_ERROR_SIZE_INSUFFICIENT if capacity too small.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateDisplayRenderingModesEXT)(
    XrSession session,
    uint32_t modeCapacityInput,
    uint32_t *modeCountOutput,
    XrDisplayRenderingModeInfoEXT *modes);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateDisplayRenderingModesEXT(
    XrSession                           session,
    uint32_t                            modeCapacityInput,
    uint32_t                           *modeCountOutput,
    XrDisplayRenderingModeInfoEXT      *modes);
#endif

// ---- v10: Unified Display Mode Events ----

#define XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT        ((XrStructureType)1000999010)
#define XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT ((XrStructureType)1000999011)

/*!
 * @brief Event fired when the active rendering mode changes.
 *
 * Pushed by xrRequestDisplayRenderingModeEXT on every actual mode change.
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataRenderingModeChangedEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    uint32_t                    previousModeIndex;
    uint32_t                    currentModeIndex;
} XrEventDataRenderingModeChangedEXT;

/*!
 * @brief Event fired when the physical display hardware state changes.
 *
 * Pushed by xrRequestDisplayRenderingModeEXT only when the hardware 3D
 * state flips (i.e., when switching between modes with different
 * hardwareDisplay3D values).
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataHardwareDisplayStateChangedEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrBool32                    hardwareDisplay3D;
} XrEventDataHardwareDisplayStateChangedEXT;

// xrSetSharedTextureOutputRectEXT moved to XR_EXT_win32_window_binding.h / XR_EXT_cocoa_window_binding.h (v12)

// ---- v14: Per-Mode Tracking Capability + Tracking-State Event (#441) ----

#define XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_EXT  ((XrStructureType)1000999012)
#define XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT ((XrStructureType)1000999013)

/*!
 * @brief Per-mode tracking capability — chained by the APP to each
 * XrDisplayRenderingModeInfoEXT element's next before calling
 * xrEnumerateDisplayRenderingModesEXT.
 *
 * OPT-IN HANDSHAKE: to use the chain, the app MUST pre-set each array
 * element's type to XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT (standard OpenXR
 * input convention) and set next to this struct (or a chain containing it).
 * The runtime only walks elements carrying that type — v13-and-earlier
 * binaries leave type/next uninitialized, and the runtime keeps overwriting
 * next with NULL for them, exactly as before.
 *
 * hasTracking tells whether the rendering mode consumes live eye tracking
 * (e.g., a tracked 3D mode or a "2D tracked" mode) or is fully untracked
 * (e.g., SBS/anaglyph export modes; every sim_display mode). When the ACTIVE
 * mode has hasTracking == XR_FALSE, XrViewEyeTrackingStateEXT.isTracking is
 * always XR_FALSE — regardless of tracker state. xrLocateViews still returns
 * fully populated views in every mode.
 *
 * LAYOUT-FREEZE POLICY: XrDisplayRenderingModeInfoEXT is frozen at its v13
 * layout. The runtime's enumerate fill writes array elements with its own
 * compiled stride, so appending fields (as v12/v13 did) silently corrupts app
 * binaries compiled against older headers. All future per-mode fields MUST be
 * added as chained structs like this one.
 *
 * @extends XrDisplayRenderingModeInfoEXT
 */
typedef struct XrDisplayRenderingModeTrackingInfoEXT {
    XrStructureType             type;        //!< Must be XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_EXT
    void* XR_MAY_ALIAS          next;
    XrBool32                    hasTracking; //!< Mode consumes live eye tracking
} XrDisplayRenderingModeTrackingInfoEXT;

/*!
 * @brief Event fired on every edge of the derived isTracking value.
 *
 * The runtime derives isTracking as
 *   activeMode.hasTracking && dp.is_tracking
 * and queues this event whenever the value changes — on DP-reported tracking
 * loss/recovery AND on rendering-mode switches into/out of untracked modes.
 *
 * This is the primary tracking-loss notification for MANUAL eye-tracking mode
 * (detect isTracking == XR_FALSE, run your own transition, request a 2D mode
 * when ready). It also fires in MANAGED mode, where apps may ignore it.
 * Edge detection runs in the runtime's xrLocateViews path, so a session that
 * never locates views receives no events.
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataEyeTrackingStateChangedEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrBool32                    isTracking; //!< New state
    XrEyeTrackingModeEXT        activeMode; //!< Session's MANAGED/MANUAL preference at edge time
} XrEventDataEyeTrackingStateChangedEXT;

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_DISPLAY_INFO_H
