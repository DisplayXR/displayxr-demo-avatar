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
 * @brief  Header for XR_DXR_wayland_surface_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension lets an OpenXR application provide its own Wayland surface
 * (wl_display* + wl_surface*) to the runtime on desktop Linux. When provided,
 * the runtime renders into the application's surface instead of creating its
 * own window. Sibling of XR_DXR_xlib_window_binding (X11), XR_DXR_win32_window_binding
 * (HWND), and XR_DXR_cocoa_window_binding (NSView).
 *
 * The app owns the surface lifecycle (registry, xdg-shell toplevel, configure
 * acks, the Wayland event loop); the runtime only builds its VkSurfaceKHR from
 * the pair via VK_KHR_wayland_surface. Transparency is native on Wayland — a
 * surface composites its premultiplied alpha over whatever is behind it, so
 * transparentBackgroundEnabled needs no ARGB-visual dance (unlike X11).
 *
 * Spec v2 adds the SIZE half of the binding — @ref XrWaylandSurfaceGeometryDXR
 * and @ref xrSetWaylandSurfaceGeometryDXR — because a wl_surface has no
 * intrinsic size and nobody but the application can supply one:
 *
 *   - Wayland's WSI reports `VkSurfaceCapabilitiesKHR::currentExtent ==
 *     {UINT32_MAX, UINT32_MAX}`: the compositor asks the CLIENT to choose, and
 *     the buffer the client attaches is what DEFINES the surface size. A
 *     runtime that guesses therefore does not mis-size a window — it RESIZES
 *     it. Before v2 the guess was the panel, so every Wayland session was
 *     forced fullscreen-on-panel.
 *   - The compositor-side geometry service (the `window-geometry@displayxr.org`
 *     GNOME Shell extension, docs/specs/runtime/wayland-window-geometry.md)
 *     cannot bootstrap it: Mutter only lists a window once it has a MAPPED
 *     buffer, and the first buffer is attached by the very swapchain whose
 *     size is in question.
 *   - The application always knows. It received `xdg_toplevel.configure` and
 *     acked it before `xrCreateSession`, and it receives every later configure.
 *
 * So SIZE comes from the app and POSITION keeps coming from the compositor
 * geometry service. That split is ADR-033 unchanged: both channels report
 * GEOMETRY, and the weaver still owns all phase including snapping.
 *
 * The shape is lifted from the Android sibling
 * (XR_DXR_android_surface_binding's @ref xrSetAndroidWindowGeometryDXR,
 * ADR-036 D6) for the same underlying reason: a window fact the runtime has no
 * way to observe from outside the app's own toolkit.
 */
#ifndef XR_DXR_WAYLAND_SURFACE_BINDING_H
#define XR_DXR_WAYLAND_SURFACE_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_wayland_surface_binding 1
#define XR_DXR_wayland_surface_binding_SPEC_VERSION 2
#define XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_wayland_surface_binding"

// Value from the DisplayXR provisional 1004999xxx block — decade 1004999250–259.
// Replace with an official Khronos-assigned value if the extension is
// standardized.
//
// This was originally 1004999210, which COLLIDED with
// XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR: the 210–219 decade already belonged to
// XR_DXR_display_info's v16+ additions (see this directory's README.md
// registry), not to the window-binding siblings. Both structs happened to be
// chained onto different parents, so nothing misbehaved in practice, but two
// distinct XrStructureType values must never share a number. Corrected to a
// fresh decade; the extension is at SPEC_VERSION 1 and no shipped app chains
// this struct, so the renumber breaks nothing.
#define XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999250)

// 251 = the spec-v2 geometry struct, the next free slot in this extension's own
// 250–259 decade. Verified unique against EVERY XR_TYPE_*_DXR / extended-core-
// enum value declared in this directory's XR_DXR_*.h headers (the 1004999xxx
// block is shared across all of them, which is how the 1004999210 collision
// above happened) — 1004999251 appears nowhere else in the repository.
#define XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR ((XrStructureType)1004999251)

// Only meaningful on desktop Linux (Android also reports __linux__ but has no
// Wayland desktop surface path).
#if defined(__linux__) && !defined(__ANDROID__)

// Opaque stand-ins so the header stays self-contained when <wayland-client.h>
// was not included first (same trick as the xlib sibling). wl_display/wl_surface
// are opaque structs on the client side.
struct wl_display;
struct wl_surface;

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide an
 *        external Wayland surface for session rendering on desktop Linux.
 *
 * Both wlDisplay and wlSurface must be valid; the wl_display connection must
 * outlive the session (the runtime borrows it for the lifetime of the Vulkan
 * surface).
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrWaylandSurfaceBindingCreateInfoDXR {
    XrStructureType             type;       //!< Must be XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    struct wl_display*          wlDisplay;  //!< Wayland display connection (wl_display_connect)
    struct wl_surface*          wlSurface;  //!< Wayland surface owned by the app
    //! When XR_TRUE, the runtime picks a non-opaque swapchain compositeAlpha so
    //! pixels the app writes transparent (alpha = 0) compose through to whatever
    //! is behind the surface. Native on Wayland — no ARGB visual needed. Only
    //! honored when both wlDisplay and wlSurface are valid. Sibling of
    //! XrXlibWindowBindingCreateInfoDXR::transparentBackgroundEnabled.
    XrBool32                    transparentBackgroundEnabled;
} XrWaylandSurfaceBindingCreateInfoDXR;

/*!
 * @brief The surface size (and its output's refresh) the application has
 *        committed to — chained on XrSessionCreateInfo::next, spec v2.
 *
 * A wl_surface has no size of its own: the buffer the client attaches is what
 * defines it, and Wayland's WSI answers `currentExtent == {UINT32_MAX,
 * UINT32_MAX}` precisely to say "you choose". Chaining this tells the runtime
 * which size to choose, so it sizes its WSI swapchain to the app's window
 * instead of forcing the window to the panel. See the file comment for why no
 * other party can supply it.
 *
 * Chained ALONGSIDE @ref XrWaylandSurfaceBindingCreateInfoDXR, never instead of
 * it — it is a separate structure rather than two more fields on the binding
 * because growing a published structure changes its size and breaks every app
 * compiled against spec v1.
 *
 * Optional. Omitting it, or passing 0 for a field, keeps the pre-v2 behaviour
 * for that field (panel-sized swapchain / 60 Hz), so a v1 app is unaffected.
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrWaylandSurfaceGeometryDXR {
    XrStructureType             type;   //!< Must be XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR
    const void* XR_MAY_ALIAS    next;   //!< Pointer to next structure in chain
    //! The size, in PIXELS, of the BUFFER the runtime should attach — not the
    //! logical size from `xdg_toplevel.configure`. The two are equal at desktop
    //! scale 1.0 and only there.
    //!
    //! On a fractionally-scaled desktop they differ and the difference is a
    //! resample, which destroys a lenticular interlace. A FULLSCREEN toplevel
    //! must report the `wl_output.mode` size of the output it is fullscreen on
    //! (the compositor maps that buffer onto the whole output, so buffer pixels
    //! and panel pixels line up 1:1) — e.g. declare 2880x1800 for a toplevel
    //! configured at 1728x1080 on a 166.67% desktop. A windowed toplevel has no
    //! such probe; report the configure size and accept that the weave is not
    //! 1:1 unless the desktop is at 100%.
    //!
    //! 0 = unknown, which leaves the runtime on its pre-v2 panel-sized default.
    uint32_t                    width;
    uint32_t                    height;
    //! Refresh of the `wl_output` the surface is on, in milli-hertz, exactly as
    //! `wl_output.mode` reports it (e.g. 59997 for 59.997 Hz). The runtime
    //! cannot query this itself on Wayland — the RandR path that serves the X11
    //! sibling needs an XCB connection — so without it the runtime falls back to
    //! a hardcoded 60 Hz. 0 = unknown, keep that default.
    uint32_t                    refreshMilliHertz;
} XrWaylandSurfaceGeometryDXR;

/*!
 * @brief Republish the surface geometry mid-session.
 *
 * The application calls this from its frame loop for every
 * `xdg_toplevel.configure` whose size DIFFERS from the one in force — after
 * acking it, never before. The runtime records the value and re-creates its WSI
 * swapchain at the new size on the next frame, the same way the X11 leg follows
 * an `xcb` window resize. The runtime de-duplicates, so calling it every frame
 * with an unchanged size is a cheap no-op.
 *
 * Position is NOT a parameter: on Wayland it comes from the compositor-side
 * geometry service, not from the client, which is never told where it is.
 *
 * @param session           A session created with a chained
 *                          @ref XrWaylandSurfaceBindingCreateInfoDXR.
 * @param width             New surface width in pixels; must be non-zero.
 * @param height            New surface height in pixels; must be non-zero.
 * @param refreshMilliHertz New refresh in milli-hertz, or 0 to leave the
 *                          session's current value alone.
 *
 * @return XR_SUCCESS, XR_ERROR_HANDLE_INVALID (not a live session),
 *         XR_ERROR_VALIDATION_FAILURE (zero width or height, or the session was
 *         not created with a Wayland surface binding) or
 *         XR_ERROR_FUNCTION_UNSUPPORTED (the extension was not enabled at
 *         xrCreateInstance).
 */
XRAPI_ATTR XrResult XRAPI_CALL
xrSetWaylandSurfaceGeometryDXR(XrSession session, uint32_t width, uint32_t height, uint32_t refreshMilliHertz);

typedef XrResult(XRAPI_PTR *PFN_xrSetWaylandSurfaceGeometryDXR)(XrSession session,
                                                                uint32_t width,
                                                                uint32_t height,
                                                                uint32_t refreshMilliHertz);

#endif // defined(__linux__) && !defined(__ANDROID__)

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_WAYLAND_SURFACE_BINDING_H
