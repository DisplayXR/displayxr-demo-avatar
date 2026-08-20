// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not.
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_android_surface_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension lets an OpenXR application provide its own Android Surface
 * (an `ANativeWindow*`, or the Java `android.view.Surface` it came from) to
 * the runtime. When provided, the runtime composites and weaves into the
 * application's surface instead of spawning a SurfaceView of its own.
 * Sibling of XR_DXR_win32_window_binding (HWND),
 * XR_DXR_cocoa_window_binding (NSView), XR_DXR_xlib_window_binding (X11) and
 * XR_DXR_wayland_surface_binding (wl_surface).
 *
 * Why it is not optional on Android (ADR-036 D2, runtime#1037): the
 * runtime-spawned SurfaceView is added straight to the WindowManager, so it
 * has no `ViewParent` — and `SurfaceView.onAttachedToWindow` dereferences one
 * on the freeform/translucent path, which crashes the app the moment it is
 * placed in a multi-window (freeform / split-screen) task. An app that owns
 * its own SurfaceView has a ViewParent by construction. The runtime keeps its
 * self-spawned SurfaceView only as the `_hosted` (fullscreen) fallback for an
 * app that chains nothing.
 *
 * Two runtime functions accompany the create-info struct, because both facts
 * they carry CHANGE during a session on Android and neither has a callback the
 * runtime can hook from outside the app's own View hierarchy:
 *
 *   - @ref xrSetAndroidSurfaceDXR — the surface is destroyed and recreated on
 *     every background/resume (`surfaceDestroyed` / `surfaceCreated`, or
 *     native_app_glue's `APP_CMD_TERM_WINDOW` / `APP_CMD_INIT_WINDOW`). The
 *     app republishes it; the runtime rebuilds its VkSurfaceKHR + swapchain
 *     and pauses/resumes the display processor across the gap.
 *   - @ref xrSetAndroidWindowGeometryDXR — a pure window MOVE raises no
 *     resize: `WindowFrames.didFrameSizeChange` compares w/h only, so the move
 *     goes out as a `oneway IWindow.moved` with no layout, no invalidate and no
 *     public callback, while SurfaceFlinger has already repositioned the layer
 *     with the OLD buffer. The 3D weave's interlace phase and the per-window
 *     Kooima frustum are both referenced to the window's on-panel origin, so
 *     the app publishes that origin itself (cheaply, once per frame, from a
 *     `Choreographer` callback). ADR-036 D6; ADR-033 is unchanged — this
 *     reports GEOMETRY, the weaver still owns all phase including snapping.
 *
 * TRAP: an OEM that applies the `OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS` compat
 * change makes `View.getLocationOnScreen()` return WINDOW-relative
 * coordinates, so every window would report (0,0) with no error anywhere. The
 * opt-out is per-APP and therefore belongs in the application's manifest:
 * `<property android:name=
 * "android.window.PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS"
 * android:value="false"/>`.
 */
#ifndef XR_DXR_ANDROID_SURFACE_BINDING_H
#define XR_DXR_ANDROID_SURFACE_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_android_surface_binding 1
#define XR_DXR_android_surface_binding_SPEC_VERSION 1
#define XR_DXR_ANDROID_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_android_surface_binding"

// XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR keeps the value published in
// the (previously unimplemented) sketch in docs/specs/extensions/
// XR_DXR_display_info.md §4 — it sits in one of the two unused gaps of the
// XR_DXR_display_info decade rather than in this extension's own decade. The
// geometry struct added by this revision takes the next free decade,
// 1004999220–229. Both are recorded in this directory's README.md registry.
#define XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999005)
#define XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR ((XrStructureType)1004999220)

#if defined(__ANDROID__)

// Opaque stand-in so the header stays self-contained when <android/native_window.h>
// was not included first (same trick as the xlib/wayland siblings).
struct ANativeWindow;

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide an
 *        application-owned Android Surface for session rendering.
 *
 * At least one of @p nativeWindow and @p surface must be non-NULL. The runtime
 * takes its OWN reference on the resulting `ANativeWindow` (`ANativeWindow_acquire`)
 * and releases it when the binding is replaced or the session is destroyed, so
 * the runtime never owns the Surface's lifecycle — the application does, and
 * must keep the Java `Surface` / `SurfaceView` alive for as long as it wants
 * frames on screen.
 *
 * The application MUST NOT draw into the bound surface itself: one BufferQueue
 * has one producer, and a `Surface.lockCanvas()` poisons it for GL/VK for good.
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrAndroidSurfaceBindingCreateInfoDXR {
    XrStructureType             type;           //!< Must be XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;           //!< Pointer to next structure in chain
    //! The application's `ANativeWindow`, e.g. from `ANativeWindow_fromSurface()`,
    //! `ASurfaceHolder_getNativeWindow()`, or `android_app::window` in a
    //! NativeActivity app. NULL ⟹ derive it from @p surface.
    struct ANativeWindow*       nativeWindow;
    //! Optional `jobject` for the Java `android.view.Surface` the window came
    //! from, as an opaque pointer (the header stays JNI-free). When
    //! @p nativeWindow is NULL the runtime resolves it with
    //! `ANativeWindow_fromSurface()`. Should be a global reference if provided.
    void*                       surface;
    //! Initial on-screen origin of the surface, in physical display pixels of
    //! the CURRENT rotation (`View.getLocationOnScreen()`). Seeds the weave
    //! phase and the per-window Kooima before the first
    //! @ref xrSetAndroidWindowGeometryDXR call; 0,0 is a valid fullscreen value.
    int32_t                     screenOffsetX;
    int32_t                     screenOffsetY;
    //! When XR_TRUE the runtime configures the bound surface for translucent
    //! composition, so pixels the app writes with alpha = 0 compose through to
    //! whatever SurfaceFlinger has behind the layer. Sibling of
    //! XrWin32WindowBindingCreateInfoDXR::transparentBackgroundEnabled. The app
    //! is responsible for requesting a translucent `SurfaceHolder` format.
    XrBool32                    transparentBackgroundEnabled;
} XrAndroidSurfaceBindingCreateInfoDXR;

/*!
 * @brief The bound surface's live on-screen rectangle.
 *
 * @p windowRect is in physical screen pixels of the CURRENT rotation, y down,
 * origin inclusive of any caption. @p panelExtent is the full panel extent in
 * that SAME rotation (`Display.getRealSize()`), which the runtime cannot derive
 * on its own: its display info describes the panel in its NATURAL orientation,
 * and a sub-panel window fits inside both orderings, so "which way is the panel
 * held" is genuinely ambiguous from the rect alone.
 */
typedef struct XrAndroidWindowGeometryDXR {
    XrStructureType             type;         //!< Must be XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR
    const void* XR_MAY_ALIAS    next;         //!< Pointer to next structure in chain
    XrRect2Di                   windowRect;   //!< On-screen rect, current rotation, physical px
    XrExtent2Di                 panelExtent;  //!< Panel extent in the same rotation, physical px
    int32_t                     displayId;    //!< `Display.getDisplayId()`; 0 = default display
} XrAndroidWindowGeometryDXR;

/*!
 * @brief Republish (or drop) the application-owned Surface mid-session.
 *
 * @param session The session created with a chained
 *        @ref XrAndroidSurfaceBindingCreateInfoDXR.
 * @param binding The new surface, or NULL (or a binding whose @p nativeWindow
 *        and @p surface are both NULL) to report SURFACE LOST. On loss the
 *        runtime tears its VkSurfaceKHR down and pauses the display processor;
 *        on the next non-NULL call it rebuilds and resumes. Only
 *        @p nativeWindow / @p surface / @p screenOffset* are read — the
 *        transparency opt-in is fixed at session create.
 *
 * Idempotent: republishing the same window is a cheap no-op.
 *
 * @return XR_SUCCESS, XR_ERROR_HANDLE_INVALID, XR_ERROR_VALIDATION_FAILURE or
 *         XR_ERROR_FUNCTION_UNSUPPORTED.
 */
XRAPI_ATTR XrResult XRAPI_CALL
xrSetAndroidSurfaceDXR(XrSession session, const XrAndroidSurfaceBindingCreateInfoDXR *binding);

/*!
 * @brief Publish the bound surface's current on-screen geometry.
 *
 * Cheap enough to call once per frame; the runtime de-duplicates. Calling it
 * from a `Choreographer` callback is the recommended shape — see the file
 * comment for why no Android callback reports a pure window move.
 *
 * @return XR_SUCCESS, XR_ERROR_HANDLE_INVALID, XR_ERROR_VALIDATION_FAILURE or
 *         XR_ERROR_FUNCTION_UNSUPPORTED.
 */
XRAPI_ATTR XrResult XRAPI_CALL
xrSetAndroidWindowGeometryDXR(XrSession session, const XrAndroidWindowGeometryDXR *geometry);

typedef XrResult(XRAPI_PTR *PFN_xrSetAndroidSurfaceDXR)(XrSession session,
                                                        const XrAndroidSurfaceBindingCreateInfoDXR *binding);
typedef XrResult(XRAPI_PTR *PFN_xrSetAndroidWindowGeometryDXR)(XrSession session,
                                                               const XrAndroidWindowGeometryDXR *geometry);

#endif // defined(__ANDROID__)

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_ANDROID_SURFACE_BINDING_H
