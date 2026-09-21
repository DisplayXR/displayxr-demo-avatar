// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux entry point for the DisplayXR avatar demo.
 *
 * The avatar demo has per-platform entry points — macos/main.mm (Cocoa) and
 * windows/main.cpp (Win32). This is the Linux sibling. It drives the SAME
 * vendor-neutral, cross-platform ModelRenderer (model_common/) through a
 * Vulkan + OpenXR frame loop. windows/main.cpp is the parity spec for
 * everything above the renderer.
 *
 * Windowing — APP-OWNED X11 WINDOW (transparent overlay, runtime#757). The app
 * creates a 32-bit ARGB X11 window and hands it to the runtime via
 * XR_DXR_xlib_window_binding with transparentBackgroundEnabled, so transparent
 * pixels compose through to the desktop — the transparent/click-through avatar
 * (the Lenovo use case). Falls back to hosted-NULL (graphics binding chained
 * straight in, runtime self-creates its window) when no X server / the
 * extension is absent (e.g. headless CI), where the Local2D bubble is
 * suppressed because xrEndFrame rejects it without an external window.
 *
 * What this leg does: display-zone framing (the avatar in the bottom 75%,
 * XR_DXR_display_zones), the Local2D speech bubble in the top 25%, auto-fit
 * against the zone rect, dynamic recenter, XShape silhouette click-through
 * (clickthrough.cpp), XR_DXR_depth_budget v1 + v3, rendering-mode adoption and
 * switching, a client-owned RMB window drag phase-snapped through
 * xrWeaveSnapWindowRectDXR (XR_DXR_weave, runtime#1588), and the Windows leg's
 * keyboard + mouse controls (see the control table on PumpXEvents).
 *
 * What it still does not do: no on-panel HUD or toast chips (the bubble is the
 * only overlay), no XR_DXR_mcp_tools agent surface, no drag-and-drop model
 * load (Ctrl+O opens a zenity picker instead), no camera-rig round-trip (C).
 *
 * Environment:
 *   AVATAR_TRANSPARENT=0        start opaque (Ctrl+T toggles at runtime)
 *   AVATAR_WINDOW=WxH+X+Y       override the window rect; X,Y are absolute
 *                               virtual-desktop px. W×H alone re-centres on
 *                               the 3D panel.
 *   DXR_RECENTER_PIN=XYZ|XY|Z|- initial recenter pins (P then X/Y/Z at runtime)
 *   DXR_ZONES_VALIDATE=1        strict zone locate/submit pairing check
 *   DXR_DUMP_BUBBLE=1           dump the rasterised bubble to /tmp
 *   DXR_AVATAR_SIL_TEXEL_PX / _DILATE / _ALPHA, DXR_VK_NO_HOST_CACHED=1
 *                               click-through silhouette levers
 *                               (see clickthrough.cpp)
 */

#include <vulkan/vulkan.h>

// Xlib FIRST so XR_DXR_xlib_window_binding.h binds the real Display*/Window
// types (it provides stand-ins only when Xlib.h was not included). Needed for
// the app-owned-window path below.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
// XkbSetDetectableAutoRepeat: without it X11 synthesises a KeyRelease before
// every auto-repeat KeyPress, so a held W/A/S/D key stutters between "down" and
// "up" and the pan crawls. Part of libX11 — no extra link dependency.
#include <X11/XKBlib.h>
// Xrandr: query the panel (primary / largest non-eDP output) rect so the
// portrait window opens centered on the 3D display, not the laptop panel.
#include <X11/extensions/Xrandr.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// App-provided-window path: the avatar creates a 32-bit ARGB X11 window and
// hands it to the runtime via XR_DXR_xlib_window_binding with
// transparentBackgroundEnabled — the transparent/click-through overlay (the
// Lenovo use case). Falls back to hosted-NULL when no X server is available.
#include <openxr/XR_DXR_xlib_window_binding.h>
// Adaptive tiling: XR_DXR_display_info gives the physical panel dims + the
// per-view recommended scale (recommendedViewScaleX/Y). Enabling it ALSO
// switches the runtime off the legacy-app compromise view-scale path (the
// "app did not enable XR_DXR_display_info → compromise view scale" WARN) — so
// the compositor tiles window×scaleXY (window-relative Kooima), exactly like
// cube_handle_vk_linux. This is what makes the avatar a proper EXTENSION app.
#include <openxr/XR_DXR_display_info.h>
// Display-centric view rig: the runtime returns render-ready XrView{pose,fov}
// (camera at the eye, off-axis Kooima FOV) so the app doesn't hand-roll the view
// math. This is how modelviewer/gauss/cube-handle frame correctly on the
// native-VK path; the avatar consumes it the same way.
#include <openxr/XR_DXR_view_rig.h>
// Local 3D zone / Local2D composition layer — the flat 2D speech bubble in the
// top 25% band, composited post-weave so the avatar keeps weaving in the bottom
// 75%. Mirrors the macOS/Windows peers' speech-bubble layer.
#include <openxr/XR_DXR_local_3d_zone.h>
// Runtime #1486/#1500: PRIMARY_STEREO reports exactly 2 views and xrEndFrame
// rejects a projection layer that carries more. DxrSelectViewConfigType picks
// XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR when the runtime advertises
// it; DxrClampSubmitViewCount is the INV-3.1 submit gate.
#include "../common/dxr_view_config.h"
#include "dxr_submit_views.h"
// Display zones (ADR-027): the tiger-zone. The 3D avatar renders rig-framed INTO
// one bottom-75% zone rect (no squish, 3D content kept out of the top band); the
// speech bubble is the Local2D layer in the top 25%. XrDisplayZoneDXR is chained
// on xrLocateViews + the projection layer; caps + per-zone view size come from
// xrGetDisplayZone{Capabilities,RecommendedViewSize}DXR. Same sequence as
// windows/main.cpp + the proven cube_zones_vk_linux reference.
#include <openxr/XR_DXR_display_zones.h>
// Multi-view atlas capture (the I key) — the same runtime-owned snapshot the
// Windows leg takes, used by /make-app-logos and for eyeballing the tile layout
// without a 3D panel.
#include <openxr/XR_DXR_atlas_capture.h>
// XR_DXR_depth_budget (ADR-040): the runtime's advisory rear depth budget, read
// off XrViewState at xrLocateViews. displayxr-common's clip_policy.h is the ONE
// place that turns it (or its absence) into near/far + the shader far-cull, and
// content_mask.h reduces the silhouette to the v3 occupancy grid.
#include <openxr/XR_DXR_depth_budget.h>
// XR_DXR_weave carries xrWeaveSnapWindowRectDXR — the drag-time window-origin
// phase snap the client-owned RMB drag routes every step through (runtime#1588).
// Optional: absent, the drag is merely unsnapped.
#include <openxr/XR_DXR_weave.h>
#include "clip_policy.h"
#include "content_mask.h"

#include "clickthrough.h" // XShape silhouette click-through for the overlay

#include <cmath>
#include <cstring>
#include <strings.h>   // strncasecmp — eDP output-name filter in GetPanelRect
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>

#include "model_renderer.h"
#include "recenter_control.h"  // dynamic-recenter per-axis pins (DXR_RECENTER_PIN on Linux)
#include "model_loader.h"
#include "model_vulkan_utils.h" // modelCreateBuffer / ModelBuffer (bubble staging)

// displayxr-common (via sr_common_base → displayxr::common): remap
// GL-convention clip-depth ([-1,1]) to Vulkan's [0,1]. mat4_from_xr_fov emits
// GL depth; without this remap, geometry nearer than the mid-range crossover is
// near-clipped in Vulkan (the ZDP-anchored near sits close to the content).
// Header-only static inline — same include the win/mac peers + cube_handle use.
#include "projection_depth.h"
#include "auto_fit.h"            // dxr::AutoFitVHeight — shared load-time framing rule

#include "stb_truetype.h"        // CPU text rasterizer for the speech bubble
                                 // (impl defined in stb_truetype_impl.cpp)

// ============================================================================
// Logging
// ============================================================================
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call)                                                     \
    do {                                                                   \
        XrResult _r = (call);                                              \
        if (XR_FAILED(_r)) {                                               \
            LOG_ERROR("%s failed: %d", #call, (int)_r);                    \
            return false;                                                  \
        }                                                                  \
    } while (0)

#define VK_CHECK(call)                                                     \
    do {                                                                   \
        VkResult _r = (call);                                              \
        if (_r != VK_SUCCESS) {                                            \
            LOG_ERROR("%s failed: %d", #call, (int)_r);                    \
            return false;                                                  \
        }                                                                  \
    } while (0)

static volatile bool g_running = true;
static ModelRenderer g_modelRenderer;

// stb_image_write (defined in stb_truetype_impl.cpp) — only used by the optional
// DXR_DUMP_BUBBLE debug dump. STBIWDEF is `extern "C"` in C++, so the definition
// linked from that TU matches this declaration.
extern "C" int stbi_write_png(char const* filename, int w, int h, int comp,
                              const void* data, int stride_in_bytes);

// ============================================================================
// Speech bubble — Local2D layer (XR_DXR_local_3d_zone)
// ============================================================================
// A flat 2D nameplate pill in the top 25% band, composited post-weave as a
// Local2D layer so the avatar keeps weaving in the bottom 75%. The rounded panel
// + word-wrapped greeting are CPU-rasterized (stb_truetype replaces the macOS
// CoreText / Windows DirectWrite path), uploaded to an app-owned window-space
// swapchain, then mapped sub-rect → full band. Mirrors macos/main.mm's
// RenderBubbleBitmap + CreateBubbleSwapchain + the bubble submission block.
static constexpr float kAvatarCanvasFrac = 0.75f;   // avatar = bottom 75%, bubble = top 25%
static const uint32_t  kBubbleTexW = 2048;          // generous ~4:1 fixed texture
static const uint32_t  kBubbleTexH = 512;
static const char*     kBubbleText = "Hi there! I'm Leo, your friendly 3D desktop avatar.";

static bool g_hasLocal3DZone = false;               // XR_DXR_local_3d_zone enabled on the instance

static XrSwapchain g_bubbleSwapchain = XR_NULL_HANDLE;
static int64_t     g_bubbleFormat = 0;
static std::vector<XrSwapchainImageVulkanKHR> g_bubbleImages;
static ModelBuffer g_bubbleStaging = {};            // host-visible RGBA8 upload buffer
static void*       g_bubbleStagingMapped = nullptr;
static bool        g_bubbleReady = false;           // swapchain + staging created

// ============================================================================
// Tiger-zone — XR_DXR_display_zones (ADR-027)
// ============================================================================
// The 3D avatar renders rig-framed INTO one bottom-75% display-zone rect (so it
// stops squishing and 3D content stays out of the top band); the speech bubble
// is the Local2D layer in the top 25%. Enabled only when the runtime advertises
// XR_DXR_display_zones + caps.supported (else the app falls back to the full-tile
// + Local2D path). Mirrors windows/main.cpp + cube_zones_vk_linux.
static bool g_hasDisplayZones = false;   // ext enabled + prereqs (view_rig + local_3d_zone)
static bool g_zonesActive = false;       // caps.supported + zone swapchain created
static bool g_zonesAttempted = false;    // one-shot activation guard
static PFN_xrGetDisplayZoneCapabilitiesDXR      g_pfnGetZoneCaps = nullptr;
static PFN_xrGetDisplayZoneRecommendedViewSizeDXR g_pfnGetZoneViewSize = nullptr;
static XrSwapchain g_zoneSwapchain = XR_NULL_HANDLE;   // pre-sized to the atlas envelope
static std::vector<XrSwapchainImageVulkanKHR> g_zoneImages;
static uint32_t g_zoneSwW = 0, g_zoneSwH = 0;

// DXR_ZONES_VALIDATE=1 chains the strict locate/submit pairing validate bit on
// xrEndFrame (bring-up diagnostics). Default: nothing chained = AUTO wish
// (feathered bottom-75%), matching windows/main.cpp + cube_zones_vk_linux.
static bool AvatarZonesValidate() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return e;
}

// ============================================================================
// Matrix helpers (column-major, m[col*4 + row]) — verbatim shape from the
// runtime's cube_handle_vk_linux; only the view/projection builders are needed.
// ============================================================================
static void mat4_from_xr_fov(float* m, const XrFovf& fov, float nearZ, float farZ) {
    float left = nearZ * tanf(fov.angleLeft);
    float right = nearZ * tanf(fov.angleRight);
    float top = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);

    float w = right - left;
    float h = top - bottom;

    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f * nearZ / w;
    m[5]  = 2.0f * nearZ / h;
    m[8]  = (right + left) / w;
    m[9]  = (top + bottom) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* m, const XrPosef& pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
    float r01 = 2.0f * (qx * qy - qz * qw);
    float r02 = 2.0f * (qx * qz + qy * qw);
    float r10 = 2.0f * (qx * qy + qz * qw);
    float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
    float r12 = 2.0f * (qy * qz - qx * qw);
    float r20 = 2.0f * (qx * qz - qy * qw);
    float r21 = 2.0f * (qy * qz + qx * qw);
    float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    float tx = -(r00 * px + r10 * py + r20 * pz);
    float ty = -(r01 * px + r11 * py + r21 * pz);
    float tz = -(r02 * px + r12 * py + r22 * pz);

    m[0]  = r00;  m[4]  = r10;  m[8]  = r20;  m[12] = tx;
    m[1]  = r01;  m[5]  = r11;  m[9]  = r21;  m[13] = ty;
    m[2]  = r02;  m[6]  = r12;  m[10] = r22;  m[14] = tz;
    m[3]  = 0.0f; m[7]  = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}


// Auto-fit — SIZE THE VIRTUAL DISPLAY TO THE MODEL (matches windows/macOS
// ApplyAutoFitForLoadedScene). The model renders at its NATIVE scale; the display
// view rig is placed at the model center with the virtualDisplayHeight that caps
// it at dxr::kAutoFitDefaultFill (80%) of the 3D zone in BOTH axes — the shared
// rule in displayxr-common's auto_fit.h. No model scaling, no offsets — the
// runtime rig + eye positions own the view pose.
static constexpr float kFallbackVHeightM = 1.5f;         // degenerate-extent fallback (win/mac parity)

// Live client size of the avatar window (px), tracked so ComputeAutoFit — which
// has no session handle — can fit against the zone rect. Seeded at window
// creation, refreshed on ConfigureNotify. 0 = unknown, which degrades the fit to
// height-only. Caveat: on the app-owned path the runtime may reposition/resize
// the overlay without a ConfigureNotify, so this can lag the real geometry (the
// per-frame consumers re-query via XGetGeometry; load-time framing does not).
static unsigned int g_clientPxW = 0, g_clientPxH = 0;
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};        // rig pose position (model AABB center)
static float g_fitVHeight = kFallbackVHeightM;           // rig virtualDisplayHeight (model height × comfort)
static bool g_fitValid = false;

// Dynamic-recenter pins. Default X Y (Z off) matches the Windows avatar; P
// arms and X/Y/Z toggle an axis (Windows parity), or DXR_RECENTER_PIN=XYZ|XY|Z|-
// sets them up front.
static dxr::RecenterControl g_recenter;

// ============================================================================
// Interactive state — the Windows leg's controls, ported to X11
// ============================================================================
// windows/main.cpp keeps these in displayxr-common's InputState, which is a
// Win32 header (UINT/WPARAM/LPARAM) and cannot be reused here. The X11 pump
// below (PumpXEvents) writes these directly; the render loop reads them. Single
// threaded — unlike Windows, there is no separate message-pump thread, so no
// lock is needed and none is taken.

//! Ctrl+T. Drives renderEye's transparentBg AND dxr::ResolveClipPlanes'
//! `transparent` (an opaque frame must never be rear-clipped — see clip_policy.h).
//! The SESSION's transparency is fixed at xrCreateSession and cannot follow this
//! toggle; what changes is what the app draws, exactly as on Windows.
static bool g_transparentBg = true;

//! Mouse wheel / SPACE. The rig's virtual display height is divided by this, so
//! wheel-up (>1) shrinks the virtual display and the avatar grows. Mirrors
//! windows/main.cpp's `virtualDisplayHeight / viewParams.scaleFactor`.
static float g_zoomFactor = 1.0f;

//! '-' / '+' (and Shift+wheel). Driven in lockstep into the rig's ipdFactor and
//! parallaxFactor, the same "one 3D-effect strength knob" the shared Windows
//! input handler implements.
static float g_ipdFactor = 1.0f;

//! WASD / QE pan+dolly and the double-click focus target, as an offset from the
//! auto-fit centre. Added on top of the per-axis recenter anchor in
//! ComputeRigPosition — identical composition to windows/main.cpp's
//! (cameraPos - fitCentre) offset.
static float g_panX = 0.0f, g_panY = 0.0f, g_panZ = 0.0f;
static bool  g_keyW = false, g_keyA = false, g_keyS = false, g_keyD = false;
static bool  g_keyQ = false, g_keyE = false;

//! Double-click focus (windows/main.cpp's teleport): ease the pan offset toward
//! the picked surface point instead of snapping.
static bool  g_focusActive = false;
static float g_focusTarget[3] = {0.0f, 0.0f, 0.0f};

//! B / F11 window state. Borderless (undecorated) is the default: a title bar
//! captures input and defeats the XShape click-through, and it looks wrong for a
//! floating overlay. Decorating clears the input shape so the whole framed
//! window is interactive, exactly like the Windows `SetWindowRgn(NULL)` branch.
static bool g_decorated = false;
static bool g_fullscreen = false;

//! The virtual display height the rig should use this frame (auto-fit ÷ zoom).
static float CurrentVHeight() {
    const float base = g_fitValid ? g_fitVHeight : kFallbackVHeightM;
    const float z = (g_zoomFactor > 1.0e-3f) ? g_zoomFactor : 1.0e-3f;
    return base / z;
}

static void ComputeAutoFit() {
    float center[3], extent[3];
    if (!g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent)) {
        g_fitValid = false;
        return;
    }
    g_fitCenter[0] = center[0];
    g_fitCenter[1] = center[1];
    g_fitCenter[2] = center[2];
    // Fit the WIDTH as well as the height: vHeight is the VIRTUAL DISPLAY
    // height, so a larger value renders the model smaller and taking the binding
    // axis crops nothing. The viewport is the 3D ZONE — the bottom
    // kAvatarCanvasFrac of the client rect (the top band is the Local2D speech
    // bubble and never holds model pixels), the same rect the zone render path
    // submits.
    const float zoneW = (float)g_clientPxW;
    const float zoneH = (float)g_clientPxH * kAvatarCanvasFrac;
    const float zoneAspect = (zoneH > 0.0f) ? zoneW / zoneH : 0.0f;
    const float heightFit = extent[1] / dxr::kAutoFitDefaultFill;
    float vh = dxr::AutoFitVHeight(extent[0], extent[1], zoneW, zoneH);
    bool widthBound = vh > heightFit * 1.0001f;
    const char* fitBox = "union";

    // Phase 2 of the 80% rule: when the model was loaded with the all-clips
    // bounds sweep (every load path sets it), SIZE the rig from the ACTIVE
    // clip's swept box — the subject as the user actually sees it — and keep the
    // union box only as the safety envelope. Sizing off the union would shrink
    // the subject by however far the widest/tallest OTHER clip reaches (a wave
    // clip's raised arm, a walk clip's root travel), which reads as "the avatar
    // loaded small and distant". A model with no clips, or one loaded without
    // the sweep, keeps the union fit above unchanged.
    float aMin[3], aMax[3];
    if (g_modelRenderer.getActiveClipBounds(aMin, aMax)) {
        const float rawW = aMax[0] - aMin[0];
        const float rawH = aMax[1] - aMin[1];
        const float subjW = (rawW > 1e-5f) ? rawW : 1e-5f;
        const float subjH = (rawH > 1e-5f) ? rawH : 1e-5f;
        const float subjHeightFit = subjH / dxr::kAutoFitDefaultFill;
        vh = dxr::AutoFitVHeight(subjW, subjH, zoneW, zoneH);
        widthBound = vh > subjHeightFit * 1.0001f;
        fitBox = "active-clip";
        g_fitCenter[0] = 0.5f * (aMin[0] + aMax[0]);
        g_fitCenter[1] = 0.5f * (aMin[1] + aMax[1]);
        g_fitCenter[2] = 0.5f * (aMin[2] + aMax[2]);
        // Clamp the vertical placement so the UNION floor stays in frame: the
        // feet of whichever clip dips lowest must never truncate at the zone
        // edge, even though the active clip decided the size.
        const float unionFloor = center[1] - 0.5f * extent[1];
        const float pad = vh * 0.02f;
        const float maxCy = unionFloor - pad + 0.5f * vh;
        const bool clamped = g_fitCenter[1] > maxCy;
        if (clamped) g_fitCenter[1] = maxCy;
        LOG_INFO("Auto-fit (active clip): subject=%.4fx%.4f centre=(%.3f,%.3f,%.3f) "
                 "unionFloor=%.4f maxCy=%.4f%s",
                 subjW, subjH, g_fitCenter[0], g_fitCenter[1], g_fitCenter[2],
                 unionFloor, maxCy, clamped ? " (clamped)" : "");
    }
    if (!(vh > 1e-3f)) vh = kFallbackVHeightM; // degenerate (thin) extent
    g_fitVHeight = vh;
    g_fitValid = true;
    LOG_INFO("Auto-fit: center=(%.3f,%.3f,%.3f) extent=(%.3f,%.3f,%.3f) "
             "vHeight=%.3f (%s box, %s-bound, zone=%.0fx%.0f aspect=%.3f fill=%.2f)",
             center[0], center[1], center[2], extent[0], extent[1], extent[2], vh,
             fitBox, widthBound ? "width" : "height",
             zoneW, zoneH, zoneAspect, dxr::kAutoFitDefaultFill);
}

// Rig position for this frame (concept 2, dynamic recenter): a pinned axis tracks
// the smoothed animated centroid; an unpinned axis stays at the initial fit
// centre. The user's pan/dolly (A/D → X, Q/E → Y, W/S → Z, plus the double-click
// focus target) adds on top of both — the exact composition windows/main.cpp
// performs with (cameraPos - fitCentre), including its kPanSign flip on X so D
// slides the avatar to the viewer's right. Default pins X Y → X,Y track the
// centroid, Z stays at the framed depth, matching the Windows avatar.
static void ComputeRigPosition(float out[3]) {
    out[0] = g_fitCenter[0];
    out[1] = g_fitCenter[1];
    out[2] = g_fitCenter[2];
    float anchor[3];
    if (g_fitValid && g_modelRenderer.getAnimatedAnchor(anchor)) {
        const dxr::RecenterPins pins = g_recenter.pins();
        if (pins.x) out[0] = anchor[0];
        if (pins.y) out[1] = anchor[1];
        if (pins.z) out[2] = anchor[2];
    }
    static constexpr float kPanSign = -1.0f;  // D = avatar right (windows parity)
    out[0] += kPanSign * g_panX;
    out[1] += g_panY;
    out[2] += g_panZ;
}

//! The rig position BEFORE the user's pan is added — the fit centre on an
//! unpinned axis, the smoothed animated centroid on a pinned one. PickFocus
//! needs it: solving for a pan that lands the rig on a point requires knowing
//! what the pan is measured from, and with the default X+Y pins that is the
//! anchor, not g_fitCenter.
static void ComputeRigBase(float out[3]) {
    out[0] = g_fitCenter[0];
    out[1] = g_fitCenter[1];
    out[2] = g_fitCenter[2];
    float anchor[3];
    if (g_fitValid && g_modelRenderer.getAnimatedAnchor(anchor)) {
        const dxr::RecenterPins pins = g_recenter.pins();
        if (pins.x) out[0] = anchor[0];
        if (pins.y) out[1] = anchor[1];
        if (pins.z) out[2] = anchor[2];
    }
}

// Advance the keyboard pan/dolly and the double-click focus ease. One call per
// frame from the main loop. Speed is windows/main.cpp's exactly:
// 0.1 · m2v / zoom virtual units per second, where m2v converts metres of
// physical panel into the rig's virtual units — so the gesture feels the same
// however the model is scaled or the panel sized.
static void UpdateInteractive(float dt, float displayHeightM) {
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;   // first frame / a stall

    const float baseVH = g_fitValid ? g_fitVHeight : kFallbackVHeightM;
    float m2v = 1.0f;
    if (baseVH > 0.0f && displayHeightM > 0.0f) m2v = baseVH / displayHeightM;
    const float zoom = (g_zoomFactor > 1.0e-3f) ? g_zoomFactor : 1.0e-3f;
    const float step = 0.1f * m2v / zoom * dt;

    // Identity rig orientation → the movement basis is world-axis-aligned
    // (forward = -Z, right = +X, up = +Y), which is what the Windows path
    // reduces to as well: the avatar pins yaw/pitch to 0 before the move.
    if (g_keyW) g_panZ -= step;
    if (g_keyS) g_panZ += step;
    if (g_keyA) g_panX -= step;
    if (g_keyD) g_panX += step;
    if (g_keyE) g_panY += step;
    if (g_keyQ) g_panY -= step;

    // Exponential ease-out, ~90% in 0.23 s — the same curve the shared Windows
    // teleport animation uses.
    if (g_focusActive) {
        const float t = 1.0f - expf(-10.0f * dt);
        g_panX += (g_focusTarget[0] - g_panX) * t;
        g_panY += (g_focusTarget[1] - g_panY) * t;
        g_panZ += (g_focusTarget[2] - g_panZ) * t;
        const float dx = g_focusTarget[0] - g_panX;
        const float dy = g_focusTarget[1] - g_panY;
        const float dz = g_focusTarget[2] - g_panZ;
        if (dx * dx + dy * dy + dz * dz < 1.0e-8f) g_focusActive = false;
    }
}

// SPACE — return to the load-time framing: no pan, no dolly, no zoom, default
// 3D strength. Deliberately absolute (windows/main.cpp's RigResetToInitial rule)
// so repeated resets cannot drift.
static void ResetView() {
    g_panX = g_panY = g_panZ = 0.0f;
    g_zoomFactor = 1.0f;
    g_ipdFactor = 1.0f;
    g_focusActive = false;
    LOG_INFO("View reset (SPACE)");
}

// Auto-incrementing capture prefix under the user's Pictures dir, mirroring the
// Windows leg's dxr_capture::MakeCaptureAtlasPrefix (whose implementation is
// Win32/Cocoa-only). The runtime appends the layout suffix.
static std::string AtlasCapturePrefix() {
    const char* home = getenv("HOME");
    std::string dir = (home != nullptr && home[0] != '\0')
                          ? std::string(home) + "/Pictures/DisplayXR"
                          : std::string("/tmp");
    mkdir(dir.c_str(), 0755);   // best effort; a pre-existing dir returns EEXIST
    static int s_n = 0;
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s/avatar-%d", dir.c_str(), ++s_n);
    return std::string(buf);
}

// ── Double-click focus (windows/main.cpp's teleport pick) ───────────────────
// The render loop publishes the frame's CENTRE view/projection and the 3D zone
// rect; the X11 pump turns a double-click into a ray through them and eases the
// rig onto whatever surface it hits. Published, not recomputed, so the ray uses
// exactly the matrices the user clicked on.
static float   g_pickView[16] = {};
static float   g_pickProj[16] = {};
static bool    g_pickValid = false;
static int32_t g_pickZoneX = 0, g_pickZoneY = 0, g_pickZoneW = 0, g_pickZoneH = 0;

/*!
 * Unproject a client-space point through the published centre view/projection
 * and re-aim the rig at the surface it hits.
 *
 * The mouse maps over the ZONE rect, not the window — a click in the Local2D
 * speech-bubble band must not pick, exactly as on Windows.
 *
 * The ray math is hand-rolled rather than borrowed from displayxr-common's
 * display3d_unproject_ndc_to_ray, which is DirectXMath-backed and Windows-only.
 * For the column-major projection mat4_from_xr_fov emits, clip.x = P0·x + P8·z
 * and clip.w = -z, so at z = -1 the camera-space direction for a given NDC is
 * ((ndcX + P8)/P0, (ndcY + P9)/P5, -1). The GL→Vulkan depth remap rewrites only
 * the z row, so those four entries are untouched by it. The view matrix's upper
 * 3x3 is the INVERSE rig rotation, so its transpose (rows P[0..2], P[4..6],
 * P[8..10]) rotates camera space back into the world.
 */
static bool PickFocus(int clientX, int clientY) {
    if (!g_pickValid || g_pickZoneW <= 0 || g_pickZoneH <= 0) return false;
    if (clientY < g_pickZoneY) return false;    // bubble band — never picks
    const float ndcX = 2.0f * (float)(clientX - g_pickZoneX) / (float)g_pickZoneW - 1.0f;
    const float ndcY = -(2.0f * (float)(clientY - g_pickZoneY) / (float)g_pickZoneH - 1.0f);
    if (ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f) return false;

    const float* P = g_pickProj;
    const float* V = g_pickView;
    if (fabsf(P[0]) < 1.0e-8f || fabsf(P[5]) < 1.0e-8f) return false;
    const float dcx = (ndcX + P[8]) / P[0];
    const float dcy = (ndcY + P[9]) / P[5];
    const float dcz = -1.0f;

    float rayDir[3] = {
        V[0] * dcx + V[1] * dcy + V[2] * dcz,
        V[4] * dcx + V[5] * dcy + V[6] * dcz,
        V[8] * dcx + V[9] * dcy + V[10] * dcz,
    };
    const float len = sqrtf(rayDir[0] * rayDir[0] + rayDir[1] * rayDir[1] + rayDir[2] * rayDir[2]);
    if (!(len > 1.0e-8f)) return false;
    rayDir[0] /= len; rayDir[1] /= len; rayDir[2] /= len;

    // Eye position = -R · t, with t the view matrix's translation column.
    const float tx = V[12], ty = V[13], tz = V[14];
    const float rayOrigin[3] = {
        -(V[0] * tx + V[1] * ty + V[2] * tz),
        -(V[4] * tx + V[5] * ty + V[6] * tz),
        -(V[8] * tx + V[9] * ty + V[10] * tz),
    };

    float hit[3];
    if (!g_modelRenderer.pickSurface(rayOrigin, rayDir, hit)) {
        LOG_INFO("Focus: no surface under the cursor");
        return false;
    }
    // Solve for the pan that lands the RIG on the hit point. Two corrections
    // over the naive (hit - fitCentre):
    //   - the base is ComputeRigBase, not g_fitCenter. A pinned axis (X and Y
    //     by default) tracks the smoothed animated centroid, so the pan is
    //     measured from the anchor and targeting the fit centre would miss by
    //     (anchor - fitCentre) in exactly the axes that matter;
    //   - X is pre-negated because ComputeRigPosition applies kPanSign = -1 to
    //     it (so D slides the avatar right).
    // windows/main.cpp makes neither correction — it routes teleport through
    // the same kPanSign against cameraPos — so it focuses the point mirrored in
    // X and offset by the anchor. A deliberate, documented divergence.
    float base[3];
    ComputeRigBase(base);
    g_focusTarget[0] = -(hit[0] - base[0]);
    g_focusTarget[1] =  (hit[1] - base[1]);
    g_focusTarget[2] =  (hit[2] - base[2]);
    g_focusActive = true;
    LOG_INFO("Focus on surface (%.3f, %.3f, %.3f)", hit[0], hit[1], hit[2]);
    return true;
}

// ============================================================================
// OpenXR session state
// ============================================================================
struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    SwapchainInfo swapchain;

    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    uint32_t viewWidth = 0;
    uint32_t viewHeight = 0;

    // App-owned X11 window (transparent overlay). Null/0 → hosted-NULL fallback
    // (no X server, e.g. headless CI, or the xlib-binding extension is absent).
    Display* xDisplay = nullptr;
    Window xWindow = 0;
    Colormap xColormap = 0;
    unsigned int xWinW = 0, xWinH = 0; // app window size (for the click-through region)
    bool hasXlibBinding = false;       // XR_DXR_xlib_window_binding enabled on the instance
    bool usingAppWindow = false;       // the xlib binding was actually handed to xrCreateSession
    bool hasViewRig = false;           // XR_DXR_view_rig enabled — chain the display rig on locate

    // XR_DXR_display_info (adaptive tiling / extension-app path). When enabled +
    // the query succeeds (displayWidthM > 0), the per-view render TILE is
    // window×recommendedViewScale instead of the legacy-compromise fixed dims,
    // and the runtime does window-relative Kooima via the rig.
    bool hasDisplayInfo = false;       // XR_DXR_display_info enabled on the instance
    float displayWidthM = 0.0f;        // physical panel dims (m); 0 = query failed/unknown
    float displayHeightM = 0.0f;
    uint32_t displayPixelWidth = 0;    // panel resolution (px); 0 = unknown
    uint32_t displayPixelHeight = 0;
    float viewScaleX = 1.0f;           // per-view fraction of the display (from display_info)
    float viewScaleY = 1.0f;           // init to identity — only ever the extension's value
    int32_t displayScreenLeft = 0;     // 3D-panel top-left in virtual-desktop px (INV-1.3)
    int32_t displayScreenTop = 0;

    // Display rendering modes (XR_DXR_display_info). This leg does not offer a
    // mode-switch UI — it enumerates ONCE after xrCreateSession purely to learn
    // which mode the panel is already in, so the submitted view count follows
    // the runtime instead of a hardcoded 2. Mirrors the macOS leg's adoption.
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateRenderingModes = nullptr;
    uint32_t renderingModeCount = 0;
    uint32_t renderingModeViewCounts[8] = {};
    // Index of the mode the runtime reports as active
    // (XrDisplayRenderingModeInfoDXR::isActive, display_info v13).
    // UINT32_MAX = the runtime named none (pre-v13, or the enumerate failed).
    uint32_t activeRenderingMode = UINT32_MAX;
    // (v13) Whether THIS session may request each mode. False for a
    // non-controller session under a workspace, where the controller is the
    // sole mode authority — so V / 0-8 report "locked" instead of silently
    // doing nothing, and dxr::ResolveClipPlanes' `standalone` predicate is
    // derived from it exactly as windows/main.cpp derives its own.
    bool renderingModeIsRequestable[8] = {};
    PFN_xrRequestDisplayRenderingModeDXR pfnRequestRenderingMode = nullptr;

    // Eye-tracking mode toggle (T). MANAGED vs MANUAL — see
    // docs/specs/vendor/eye-tracking-modes.md in displayxr-runtime.
    PFN_xrRequestEyeTrackingModeDXR pfnRequestEyeTrackingMode = nullptr;
    XrEyeTrackingModeDXR activeEyeTrackingMode = XR_EYE_TRACKING_MODE_MANAGED_DXR;

    // Multi-view atlas capture (I) — XR_DXR_atlas_capture.
    bool hasAtlasCapture = false;
    PFN_xrCaptureAtlasDXR pfnCaptureAtlas = nullptr;

    // XR_DXR_depth_budget (#81 / ADR-040). `version` is the RUNTIME's reported
    // extensionVersion, never this app's vendored SPEC_VERSION: a v2 runtime
    // must never be handed the v3 content-mask chain it cannot parse.
    bool hasDepthBudget = false;
    uint32_t depthBudgetVersion = 0;

    // XR_DXR_weave (drag phase snap, runtime#1588). Only xrWeaveSnapWindowRectDXR
    // is used — this app is a _handle app, not a present-owning weave client.
    bool hasWeave = false;
};

// Default portrait client size (Windows parity: windows/main.cpp g_windowWidth ×
// g_windowHeight). Opened windowed + centered on the 3D panel, NOT fullscreen.
static const unsigned int kDefaultWindowW = 811;
static const unsigned int kDefaultWindowH = 1421;

// ============================================================================
// File-open dialog (O / Ctrl+O) — async zenity picker + X11 event pump
// ============================================================================

// Non-blocking zenity file-selection: fork/exec with a pipe, polled every
// frame so the XR frame loop never stalls. zenity is NOT a hard dependency —
// when absent the child exits 127 and we log a hint. On success the chosen
// model replaces the current one and auto-fit re-frames it.
static pid_t g_pickerPid = -1;
static int g_pickerFd = -1;
static std::string g_pickerBuf;

static void StartFilePicker() {
    if (g_pickerPid > 0) return; // dialog already open
    int fds[2];
    if (pipe(fds) != 0) { LOG_WARN("file picker: pipe() failed"); return; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); LOG_WARN("file picker: fork() failed"); return; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        // Start in the exe dir — the bundled models live next to the binary.
        std::string startDir;
        { char buf[PATH_MAX];
          ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
          if (n > 0) { buf[n] = '\0'; startDir = std::string(dirname(buf)) + "/"; } }
        std::string filenameArg = "--filename=" + startDir;
        const char* argv[] = {"zenity", "--file-selection", "--title=Open avatar model",
                              filenameArg.c_str(),
                              "--file-filter=3D models | *.fbx *.glb *.gltf *.obj *.stl *.FBX *.GLB",
                              "--file-filter=All files | *", nullptr};
        execvp("zenity", const_cast<char* const*>(argv));
        _exit(127); // zenity not installed
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    g_pickerPid = pid;
    g_pickerFd = fds[0];
    g_pickerBuf.clear();
    LOG_INFO("file picker: zenity dialog opened");
}

static void PollFilePicker() {
    if (g_pickerPid <= 0) return;
    char buf[512];
    ssize_t n;
    while ((n = read(g_pickerFd, buf, sizeof(buf))) > 0) g_pickerBuf.append(buf, (size_t)n);
    int status = 0;
    pid_t r = waitpid(g_pickerPid, &status, WNOHANG);
    if (r != g_pickerPid) return; // still open
    while ((n = read(g_pickerFd, buf, sizeof(buf))) > 0) g_pickerBuf.append(buf, (size_t)n);
    close(g_pickerFd);
    g_pickerFd = -1; g_pickerPid = -1;

    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 127) { LOG_WARN("file picker: zenity not installed (apt install zenity)"); return; }
    if (code != 0) { LOG_INFO("file picker: cancelled"); return; }
    while (!g_pickerBuf.empty() && (g_pickerBuf.back() == '\n' || g_pickerBuf.back() == '\r'))
        g_pickerBuf.pop_back();
    if (g_pickerBuf.empty()) return;
    if (!model_validate_file(g_pickerBuf)) { LOG_WARN("file picker: unsupported file '%s'", g_pickerBuf.c_str()); return; }
    LOG_INFO("Loading model: %s", g_pickerBuf.c_str());
    g_modelRenderer.setBoundsSweepAllClips(true);   // see the startup load in main()
    if (g_modelRenderer.loadModel(g_pickerBuf.c_str())) {
        ComputeAutoFit();
        LOG_INFO("Loaded %s", g_pickerBuf.c_str());
    } else {
        LOG_WARN("file picker: load failed for %s", g_pickerBuf.c_str());
    }
}

// Find the target panel rect (virtual-desktop px). Prefer the RandR PRIMARY
// output; else the largest connected NON-eDP/LVDS output (the Odyssey is an
// external panel, not the laptop's built-in). Fills x,y,w,h with the CRTC rect.
// Returns false if Xrandr yields nothing usable (caller centers on the screen).
static bool GetPanelRect(Display* dpy, Window root, int& x, int& y, int& w, int& h) {
    XRRScreenResources* res = XRRGetScreenResources(dpy, root);
    if (res == nullptr) return false;

    auto tryOutput = [&](RROutput out) -> bool {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, out);
        if (oi == nullptr) return false;
        bool ok = false;
        if (oi->connection == RR_Connected && oi->crtc != 0) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci != nullptr && ci->width > 0 && ci->height > 0) {
                x = ci->x; y = ci->y; w = (int)ci->width; h = (int)ci->height;
                ok = true;
            }
            if (ci != nullptr) XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
        return ok;
    };

    bool found = false;
    RROutput primary = XRRGetOutputPrimary(dpy, root);
    if (primary != 0 && tryOutput(primary)) {
        found = true;
    }
    if (!found) {
        long bestArea = 0;
        for (int i = 0; i < res->noutput; i++) {
            XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (oi == nullptr) continue;
            const bool isBuiltin = oi->name != nullptr &&
                (strncasecmp(oi->name, "eDP", 3) == 0 || strncasecmp(oi->name, "LVDS", 4) == 0);
            if (oi->connection == RR_Connected && oi->crtc != 0 && !isBuiltin) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
                if (ci != nullptr && ci->width > 0 && ci->height > 0) {
                    const long area = (long)ci->width * (long)ci->height;
                    if (area > bestArea) {
                        bestArea = area;
                        x = ci->x; y = ci->y; w = (int)ci->width; h = (int)ci->height;
                        found = true;
                    }
                }
                if (ci != nullptr) XRRFreeCrtcInfo(ci);
            }
            XRRFreeOutputInfo(oi);
        }
    }
    XRRFreeScreenResources(res);
    return found;
}

// ============================================================================
// Window-manager helpers (Motif hints + EWMH) — the X11 analogues of
// windows/main.cpp's ToggleDecoration / ToggleFullscreen / dxr::RmbWindowDrag
// ============================================================================

//! _MOTIF_WM_HINTS decorations bit. Every mainstream WM (Mutter, KWin, Xfwm,
//! i3) honours it; a WM that does not simply keeps its frame, which is a
//! cosmetic difference rather than a failure.
static void SetMotifDecorations(Display* dpy, Window win, bool decorated) {
    struct MotifWmHints { unsigned long flags, functions, decorations; long input_mode; unsigned long status; };
    MotifWmHints mwm = {2 /* MWM_HINTS_DECORATIONS */, 0, decorated ? 1UL : 0UL, 0, 0};
    Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    if (motif != None) {
        XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace, (unsigned char*)&mwm, 5);
    }
}

//! Send an EWMH _NET_WM_STATE add/remove/toggle for one state atom.
static void SetNetWmState(Display* dpy, Window win, const char* stateName, bool on) {
    Atom stateAtom = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom what = XInternAtom(dpy, stateName, False);
    if (stateAtom == None || what == None) return;
    XEvent ev = {};
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = stateAtom;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0;   // _NET_WM_STATE_ADD / _REMOVE
    ev.xclient.data.l[1] = (long)what;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;            // source indication: normal application
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
}

// ============================================================================
// Client-owned, phase-snapped window drag (RMB) — runtime#1588
// ============================================================================
//
// RIGHT-button drag moves the borderless overlay — the desktop-avatar
// convention, matching windows/main.cpp's dxr::RmbWindowDrag (and modelviewer /
// gaussiansplat). The gesture is gated on the opaque element for free: a shaped
// window never receives a button press outside its XShape input region, so the
// transparent area keeps clicking through to the desktop.
//
// WHY THE APP OWNS THE DRAG. The vendor display processor weaves at a phase
// that is a function of the window's ABSOLUTE position in physical panel
// pixels, so a window dragged across arbitrary pixels re-lands the phase every
// frame and the 3D stutters. The cure is invariance: the window only ever lands
// on the lens lattice, so the woven pattern is identical at every position the
// drag visits. Windows gets that inside the OS move loop (WM_WINDOWPOSCHANGING
// -> the DP's snap_window_rect). X11 has no equivalent: an EWMH
// _NET_WM_MOVERESIZE handoff runs the move inside the window manager's own grab
// loop and the client only learns the result afterwards — nothing can snap it
// (disproven on the DS1 under mutter; this leg used to hand the drag off that
// way and the avatar stuttered while dragged). So the app grabs the pointer
// itself and routes every step through xrWeaveSnapWindowRectDXR (XR_DXR_weave).
// The lattice is the vendor's and never leaves the DP — the app only asks
// "given I started here and want to go there, where may I land?".
//
// Do NOT round, quantize or second-guess the snapped point. Where the X screen
// can only place windows on a coarser grid (XWayland's global scale), the
// RUNTIME already offers the DP only reachable positions (runtime#1609); the
// app moves to exactly what the snap returns and merely VERIFIES the landing
// (DragCheckLanding), warning once if placement is persistently not honoured.
//
// STRICTLY OPTIONAL. Absent extension / entry point / a failing call -> one log
// line and an unsnapped drag; nothing else changes.
//
// Coordinate convention — ONE window's frame throughout, never mixed: origin,
// target, snapped result, the XMoveWindow argument and the landing read-back
// are all xWindow's absolute root origin. The vendor snap uses only the
// displacement origin -> target, so a constant offset would cancel, but mixing
// two windows' frames would not. The avatar has no header bar (borderless by
// design, on Windows too), so xWindow IS the bound, woven window.
// Mirrors the runtime's test_apps/common/dxr_linux_window.cpp X11 drag.
// ============================================================================
struct WeaveSnap {
    PFN_xrWeaveSnapWindowRectDXR pfn = nullptr;
    XrSession session = XR_NULL_HANDLE;
    int32_t w = 0, h = 0;
    bool failed = false;
    bool reported = false;

    void attach(XrInstance instance, XrSession s, uint32_t extentW, uint32_t extentH) {
        session = s; w = (int32_t)extentW; h = (int32_t)extentH; pfn = nullptr;
        if (instance == XR_NULL_HANDLE || s == XR_NULL_HANDLE) return;
        PFN_xrVoidFunction fn = nullptr;
        if (xrGetInstanceProcAddr(instance, "xrWeaveSnapWindowRectDXR", &fn) == XR_SUCCESS &&
            fn != nullptr) {
            pfn = reinterpret_cast<PFN_xrWeaveSnapWindowRectDXR>(fn);
        }
    }
    void setExtent(uint32_t nw, uint32_t nh) { w = (int32_t)nw; h = (int32_t)nh; }
    bool available() const { return pfn != nullptr; }

    //! origin/target are ABSOLUTE root (desktop) pixels — the phase is absolute.
    //! Only the offset is snapped; the extent passes through per the spec.
    bool snap(int32_t originX, int32_t originY, int32_t targetX, int32_t targetY,
              int32_t* outX, int32_t* outY) {
        if (pfn == nullptr || session == XR_NULL_HANDLE) return false;
        XrRect2Di origin = {};
        origin.offset.x = originX; origin.offset.y = originY;
        origin.extent.width = w; origin.extent.height = h;
        XrRect2Di target = origin;
        target.offset.x = targetX; target.offset.y = targetY;
        XrRect2Di snapped = {};
        const XrResult res = pfn(session, &origin, &target, &snapped);
        if (res != XR_SUCCESS) {
            if (!failed) {
                failed = true;
                LOG_WARN("xrWeaveSnapWindowRectDXR failed (%d) — the window drag falls back to "
                         "an unsnapped position for the rest of this run", (int)res);
            }
            pfn = nullptr;
            return false;
        }
        *outX = snapped.offset.x; *outY = snapped.offset.y;
        return true;
    }
};

static WeaveSnap g_weaveSnap;

// Drag bookkeeping. Single-threaded: the X pump and the render loop share one
// thread, so no lock.
static bool g_dragging = false;
static int g_dragPtrX = 0, g_dragPtrY = 0;       // pointer root position at the grab
static int g_dragOriginX = 0, g_dragOriginY = 0; // window root origin at the grab (snap origin)
static int g_dragAtX = 0, g_dragAtY = 0;         // where the window was last moved to
static uint64_t g_dragMoves = 0, g_dragSnapped = 0;

// Landing check: did the window go where the snap asked? XMoveWindow is
// asynchronous, so the previous request is compared against the window's real
// origin a pump later (just before the next move, or at the top of the next
// pump). Accumulates across drags; warns once. A snap the environment silently
// rounds away is indistinguishable from a working one unless someone reads the
// window back.
static bool g_landingPending = false;
static int g_landingWantX = 0, g_landingWantY = 0;
static uint32_t g_landingMoves = 0, g_landingDiverged = 0, g_landingWorst = 0;
static bool g_landingReported = false;

static void X11RootOrigin(Display* dpy, Window win, int* x, int* y) {
    Window child = 0;
    *x = 0; *y = 0;
    XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, x, y, &child);
}

static void DragCheckLanding(AppXrSession& xr) {
    if (!g_landingPending || xr.xDisplay == nullptr || xr.xWindow == 0) return;
    g_landingPending = false;
    int gotX = 0, gotY = 0;
    X11RootOrigin(xr.xDisplay, xr.xWindow, &gotX, &gotY);
    g_landingMoves++;
    if (gotX != g_landingWantX || gotY != g_landingWantY) {
        g_landingDiverged++;
        const uint32_t dx = (uint32_t)abs(gotX - g_landingWantX);
        const uint32_t dy = (uint32_t)abs(gotY - g_landingWantY);
        const uint32_t d = dx > dy ? dx : dy;
        if (d > g_landingWorst) g_landingWorst = d;
    }
    // Conservative, as in the runtime's placement probe: several moves and a
    // clear majority missing, so one stale read or a WM nudge cannot trip it.
    if (!g_landingReported && g_landingMoves >= 8 && g_landingDiverged * 2u >= g_landingMoves) {
        g_landingReported = true;
        LOG_WARN("drag: placement NOT honoured — %u of %u moves landed somewhere other than "
                 "the snapped origin (worst %u px). The 3D will stutter while dragging. The "
                 "runtime normally compensates for XWayland's global scale (runtime#1609); "
                 "check its log and `displayxr-cli info`, or set every output to 100%%.",
                 g_landingDiverged, g_landingMoves, g_landingWorst);
    }
}

//! Snap (or identity) + XMoveWindow to exactly the snapped point.
static void MoveWindowSnapped(AppXrSession& xr, int targetX, int targetY) {
    if (xr.xDisplay == nullptr || xr.xWindow == 0) return;
    DragCheckLanding(xr);
    int32_t sx = (int32_t)targetX, sy = (int32_t)targetY;
    const bool snapped = g_weaveSnap.snap((int32_t)g_dragOriginX, (int32_t)g_dragOriginY,
                                          (int32_t)targetX, (int32_t)targetY, &sx, &sy);
    if (!snapped) { sx = (int32_t)targetX; sy = (int32_t)targetY; }
    if (!g_weaveSnap.reported) {
        g_weaveSnap.reported = true;
        LOG_INFO("drag: snap provider %s — %s",
                 g_weaveSnap.available() ? "installed" : "ABSENT (identity)",
                 snapped ? "the display processor is offering snapped origins; each landing "
                           "is verified ('drag: placement')"
                         : "unsnapped drag (no DP lattice snap on this runtime); the drag "
                           "mechanics are unaffected");
    }
    if (sx != targetX || sy != targetY) {
        g_dragSnapped++;
        LOG_INFO("drag: raw (%d, %d) -> snapped (%d, %d)", targetX, targetY, (int)sx, (int)sy);
    }
    if (sx == g_dragAtX && sy == g_dragAtY) return;   // the lattice swallowed this step
    XMoveWindow(xr.xDisplay, xr.xWindow, sx, sy);
    XFlush(xr.xDisplay);
    g_dragAtX = sx; g_dragAtY = sy;
    g_dragMoves++;
    g_landingPending = true;
    g_landingWantX = sx; g_landingWantY = sy;
}

static void BeginWindowDrag(AppXrSession& xr, int rootX, int rootY) {
    if (g_dragging || xr.xDisplay == nullptr || xr.xWindow == 0) return;
    DragCheckLanding(xr);
    g_dragging = true;
    g_dragPtrX = rootX;
    g_dragPtrY = rootY;
    X11RootOrigin(xr.xDisplay, xr.xWindow, &g_dragOriginX, &g_dragOriginY);
    g_dragAtX = g_dragOriginX; g_dragAtY = g_dragOriginY;
    g_dragMoves = 0; g_dragSnapped = 0;
    // Explicit grab so motion OUTSIDE the window keeps arriving: the pointer
    // routinely leaves a window being dragged fast, and on this overlay it
    // leaves the XShape input region (the avatar's silhouette) within pixels.
    XGrabPointer(xr.xDisplay, xr.xWindow, False,
                 ButtonReleaseMask | PointerMotionMask | Button3MotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    LOG_INFO("drag: start (RMB) — grab origin (%d, %d), pointer (%d, %d)",
             g_dragOriginX, g_dragOriginY, g_dragPtrX, g_dragPtrY);
}

static void EndWindowDrag(AppXrSession& xr, const char* why) {
    if (!g_dragging) return;
    g_dragging = false;
    if (xr.xDisplay != nullptr) {
        XUngrabPointer(xr.xDisplay, CurrentTime);
        XFlush(xr.xDisplay);
    }
    LOG_INFO("drag: end (%s) — %llu move(s), %llu snapped away from the raw target, "
             "origin (%d, %d) -> (%d, %d); placement so far: %u of %u verified moves "
             "landed exactly",
             why, (unsigned long long)g_dragMoves, (unsigned long long)g_dragSnapped,
             g_dragOriginX, g_dragOriginY, g_dragAtX, g_dragAtY,
             g_landingMoves - g_landingDiverged, g_landingMoves);
}

// Create a 32-bit ARGB X11 window for the avatar overlay. Windowed PORTRAIT
// (811×1421, Windows parity) centered on the 3D panel — NOT fullscreen. Override
// with AVATAR_WINDOW="WxH+X+Y" (X,Y = absolute virtual-desktop px; parsed like
// cube_handle_vk_linux's DXR_CUBE_WINDOW). Returns false (leaving xDisplay null)
// when no X server / no ARGB visual is available, so the caller falls back to
// hosted-NULL. A compositing WM (GNOME/Mutter, KWin, picom) must be running for
// the desktop to show through in transparent mode.
static bool CreateAppWindow(AppXrSession& xr) {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        LOG_INFO("XOpenDisplay failed (no X server) — using hosted-NULL windowing");
        return false;
    }
    int screen = DefaultScreen(dpy);

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        LOG_INFO("No 32-bit ARGB visual — using hosted-NULL windowing");
        XCloseDisplay(dpy);
        return false;
    }

    Window root = RootWindow(dpy, screen);
    Colormap cmap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    XSetWindowAttributes attrs = {};
    attrs.colormap = cmap;
    attrs.border_pixel = 0;      // required with a non-default colormap (else BadMatch)
    attrs.background_pixel = 0;  // fully-transparent fill
    // Mouse as well as keyboard: without the Button/Motion bits the X server
    // never delivers a press to this window at all, which is why the Linux leg
    // had no RMB window-move, no wheel zoom and no double-click focus.
    attrs.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       FocusChangeMask;

    // Portrait default, centered on the 3D panel (RandR). Fall back to centering
    // on the default screen if Xrandr yields nothing.
    unsigned int w = kDefaultWindowW, h = kDefaultWindowH;
    int px = 0, py = 0;
    int prx = 0, pry = 0, prw = 0, prh = 0;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        // Authoritative: XR_DXR_display_info reports the 3D panel's desktop rect
        // (RandR-derived in the plug-in, queried before this call). Prefer it —
        // on a multi-monitor box the RandR PRIMARY is often NOT the Leia panel
        // (e.g. the laptop's own display is primary), so GetPanelRect would center
        // the avatar on the wrong (2D, non-weaving) monitor.
        prx = xr.displayScreenLeft;
        pry = xr.displayScreenTop;
        prw = (int)xr.displayPixelWidth;
        prh = (int)xr.displayPixelHeight;
        px = prx + (prw - (int)w) / 2;
        py = pry + (prh - (int)h) / 2;
        LOG_INFO("3D panel (display_info) %dx%d at (%d,%d) — centering %ux%u portrait window at (%d,%d)",
                 prw, prh, prx, pry, w, h, px, py);
    } else if (GetPanelRect(dpy, root, prx, pry, prw, prh)) {
        px = prx + (prw - (int)w) / 2;
        py = pry + (prh - (int)h) / 2;
        LOG_INFO("Panel rect (Xrandr) %dx%d at (%d,%d) — centering %ux%u portrait window at (%d,%d)",
                 prw, prh, prx, pry, w, h, px, py);
    } else {
        const int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
        px = (sw - (int)w) / 2;
        py = (sh - (int)h) / 2;
        LOG_INFO("Xrandr panel query failed — centering %ux%u on default screen %dx%d at (%d,%d)",
                 w, h, sw, sh, px, py);
    }

    // AVATAR_WINDOW="WxH+X+Y" override (X,Y absolute virtual-desktop px). Parsed
    // like cube_handle_vk_linux's DXR_CUBE_WINDOW; W×H alone re-centers.
    if (const char* wenv = getenv("AVATAR_WINDOW")) {
        unsigned int ow = 0, oh = 0; int ox = 0, oy = 0;
        int n = sscanf(wenv, "%ux%u+%d+%d", &ow, &oh, &ox, &oy);
        if (n >= 2 && ow > 0 && oh > 0) {
            w = ow; h = oh;
            if (n >= 4) {
                px = ox; py = oy;
                LOG_INFO("AVATAR_WINDOW override: %ux%u at absolute (%d,%d)", w, h, px, py);
            } else {
                // Re-center the overridden size on the same panel/screen origin.
                if (prw > 0 && prh > 0) { px = prx + (prw - (int)w) / 2; py = pry + (prh - (int)h) / 2; }
                LOG_INFO("AVATAR_WINDOW override: %ux%u (re-centered at %d,%d)", w, h, px, py);
            }
        }
    }

    Window win = XCreateWindow(dpy, root, px, py, w, h, 0, 32, InputOutput, vinfo.visual,
                               CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
    if (win == 0) {
        LOG_ERROR("XCreateWindow failed — using hosted-NULL windowing");
        XFreeColormap(dpy, cmap);
        XCloseDisplay(dpy);
        return false;
    }
    XStoreName(dpy, win, "DisplayXR Avatar");

    // Borderless by default (see SetMotifDecorations): a decorated title
    // bar/frame captures input and defeats the XShape click-through, and it
    // looks wrong for a floating overlay. B toggles it back on.
    SetMotifDecorations(dpy, win, /*decorated=*/false);

    // WM_NORMAL_HINTS with USPosition|PPosition so the WM honors the create-time
    // position instead of auto-placing (GNOME/Mutter auto-places without this).
    // Same as cube_handle_vk_linux / the runtime's own hosted-window placement.
    {
        XSizeHints hints = {};
        hints.flags = USPosition | PPosition;
        hints.x = px; hints.y = py;
        XSetWMNormalHints(dpy, win, &hints);
    }

    XMapWindow(dpy, win);
    XFlush(dpy);

    // Held W/A/S/D must not stutter — see the XKBlib include note.
    { Bool supported = False; XkbSetDetectableAutoRepeat(dpy, True, &supported); }

    // Re-assert the position after mapping — Mutter (and others) ignore the
    // create-time x/y of a freshly-mapped toplevel but honor a post-map move.
    XMoveWindow(dpy, win, px, py);
    XFlush(dpy);

    xr.xDisplay = dpy;
    xr.xWindow = win;
    xr.xColormap = cmap;
    xr.xWinW = w;
    xr.xWinH = h;
    g_clientPxW = w;   // seed the auto-fit viewport (see g_clientPxW above)
    g_clientPxH = h;
    LOG_INFO("Created %ux%u 32-bit ARGB portrait app window at (%d,%d)", w, h, px, py);
    return true;
}

// Forward declarations for the handlers the X11 pump drives.
static void RefreshRenderingModes(AppXrSession& xr, bool logTable);
static void RequestRenderingMode(AppXrSession& xr, uint32_t modeIndex, const char* via);
static void ToggleDecoration(AppXrSession& xr);
static void ToggleFullscreen(AppXrSession& xr);

/*!
 * Pump the app window's X11 events.
 *
 * This is the Linux counterpart of windows/main.cpp's WindowProc plus the
 * shared UpdateInputState it delegates to — displayxr-common's input_handler.h
 * is a Win32 header (UINT/WPARAM/LPARAM) and cannot be reused here, so the key
 * and button map is reproduced rather than shared.
 *
 * Controls, and where each one comes from on the Windows leg:
 *
 *   mouse                                      windows/main.cpp
 *   ------------------------------------------ -------------------------------
 *   right-drag            move the window      dxr::RmbWindowDrag (+ the DP
 *                         (phase-snapped)      snap in WM_WINDOWPOSCHANGING)
 *   wheel                 zoom (rig vHeight)   UpdateInputState WM_MOUSEWHEEL
 *   Shift+wheel           3D strength (ipd)    ditto, shift branch
 *   double-click          focus picked surface WM_LBUTTONDBLCLK -> teleport
 *   left-drag             (deliberately inert) yaw/pitch pinned to 0 — the
 *                                              billboard owns the heading
 *
 *   keyboard
 *   ------------------------------------------ -------------------------------
 *   Esc                   quit                 WM_KEYDOWN VK_ESCAPE
 *   Ctrl+O                open a model         OpenModelDialog
 *   Ctrl+T                transparent/opaque   transparentBgToggleRequested
 *   T                     eye-tracking mode    eyeTrackingModeToggleRequested
 *   V / 0..8              rendering mode       cycle / absolute
 *   N / K                 next clip / play     cycleClip / playPause
 *   Space                 reset the view       resetViewRequested
 *   W A S D Q E           pan / dolly          UpdateCameraMovement
 *   - / +                 3D strength          VK_OEM_MINUS / VK_OEM_PLUS
 *   P then X / Y / Z      recenter pins        dxr::RecenterControl::onKey
 *   G                     edge softening       setEdgeSoftenEnabled
 *   B                     window decoration    ToggleDecoration
 *   F11                   fullscreen           ToggleFullscreen
 *   I                     capture the atlas    xrCaptureAtlasDXR
 *
 * Deliberately NOT ported: Tab (the HUD panel the avatar does not have), C (the
 * camera/display rig round-trip, which needs displayxr-common's Windows-only
 * rig converter), M (auto-orbit — the Windows avatar forces it off every reset
 * because the face-the-viewer billboard owns the heading).
 */
static void PumpXEvents(AppXrSession& xr) {
    if (xr.xDisplay == nullptr) return;

    // Verify the previous drag step landed where the snap asked (a pump has
    // passed, so the server has placed it). No-op when nothing is pending.
    DragCheckLanding(xr);

    // RMB drag motion is coalesced across the whole drain and applied ONCE
    // afterwards — X11 delivers a MotionNotify per pointer sample and only the
    // newest one matters for an absolute move.
    bool haveDragMotion = false;
    int dragMotionRootX = 0, dragMotionRootY = 0;

    // Double-click detection. X11 has no WM_LBUTTONDBLCLK: the interval is the
    // conventional 400 ms, and the slop keeps a shaky hand from splitting a
    // double-click into two singles.
    static Time s_lastClickTime = 0;
    static int  s_lastClickX = 0, s_lastClickY = 0;
    static constexpr unsigned long kDoubleClickMs = 400;
    static constexpr int kDoubleClickSlopPx = 6;

    while (XPending(xr.xDisplay) > 0) {
        XEvent ev;
        XNextEvent(xr.xDisplay, &ev);
        switch (ev.type) {
        case KeyPress: {
            const KeySym sym = XLookupKeysym(&ev.xkey, 0);
            const bool ctrl = (ev.xkey.state & ControlMask) != 0;

            // Ctrl-chords first, so the bare-key handlers below cannot swallow
            // them (Ctrl+T must not reach the plain-T eye-tracking toggle).
            if (ctrl) {
                switch (sym) {
                case XK_o: case XK_O:
                    StartFilePicker();
                    continue;
                case XK_t: case XK_T:
                    g_transparentBg = !g_transparentBg;
                    LOG_INFO("Transparent background: %s (Ctrl+T)", g_transparentBg ? "ON" : "OFF");
                    continue;
                default: break;
                }
            }

            switch (sym) {
            case XK_Escape:
                LOG_INFO("Escape — exiting");
                if (xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
                    xrRequestExitSession(xr.session);
                } else {
                    g_running = false;
                }
                break;

            case XK_F11: ToggleFullscreen(xr); break;
            case XK_b: case XK_B: ToggleDecoration(xr); break;

            case XK_space: ResetView(); break;

            case XK_w: case XK_W: g_keyW = true; break;
            case XK_a: case XK_A: g_keyA = true; break;
            case XK_s: case XK_S: g_keyS = true; break;
            case XK_d: case XK_D: g_keyD = true; break;
            case XK_q: case XK_Q: g_keyQ = true; break;
            case XK_e: case XK_E: g_keyE = true; break;

            case XK_minus: case XK_KP_Subtract: {
                g_ipdFactor -= 0.1f;
                if (g_ipdFactor < 0.1f) g_ipdFactor = 0.1f;
                LOG_INFO("3D strength: %.2f (-)", g_ipdFactor);
                break;
            }
            case XK_plus: case XK_equal: case XK_KP_Add: {
                g_ipdFactor += 0.1f;
                if (g_ipdFactor > 1.0f) g_ipdFactor = 1.0f;
                LOG_INFO("3D strength: %.2f (+)", g_ipdFactor);
                break;
            }

            case XK_n: case XK_N:
            case XK_k: case XK_K: {
                const bool next = (sym == XK_n || sym == XK_N);
                if (next) g_modelRenderer.cycleAnimation(); else g_modelRenderer.togglePaused();
                std::string clip; int ci = 0, cn = 0; float ct = 0.0f, cd = 0.0f; bool playing = false;
                if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing)) {
                    LOG_INFO("Clip playback: %s '%s' (%d/%d) via %s",
                             playing ? "playing" : "paused", clip.c_str(), ci + 1, cn,
                             next ? "N" : "K");
                } else {
                    LOG_INFO("Clip playback: no animation clips in this model (%s ignored)",
                             next ? "N" : "K");
                }
                break;
            }

            case XK_g: case XK_G: {
                const bool now = !g_modelRenderer.edgeSoftenEnabled();
                g_modelRenderer.setEdgeSoftenEnabled(now);
                LOG_INFO("Edge-soften post-pass: %s (G)", now ? "ON" : "OFF");
                break;
            }

            case XK_t: case XK_T: {
                if (xr.pfnRequestEyeTrackingMode == nullptr || xr.session == XR_NULL_HANDLE) {
                    LOG_INFO("Eye-tracking mode toggle unavailable (no XR_DXR_display_info entry point)");
                    break;
                }
                const XrEyeTrackingModeDXR want =
                    (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                        ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
                const XrResult r = xr.pfnRequestEyeTrackingMode(xr.session, want);
                if (XR_SUCCEEDED(r)) xr.activeEyeTrackingMode = want;
                LOG_INFO("Eye tracking mode -> %s (%s)",
                         want == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
                         XR_SUCCEEDED(r) ? "OK" : "unsupported");
                break;
            }

            case XK_i: case XK_I: {
                if (!xr.hasAtlasCapture || xr.pfnCaptureAtlas == nullptr) {
                    LOG_INFO("Atlas capture unavailable (XR_DXR_atlas_capture not enabled)");
                    break;
                }
                XrAtlasCaptureInfoDXR ci = {(XrStructureType)XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                ci.stage = XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_DXR;
                // The runtime appends "_atlas_<views>_<cols>x<rows>.png", so
                // pass a bare prefix and never pre-bake the layout.
                std::string prefix = AtlasCapturePrefix();
                snprintf(ci.pathPrefix, sizeof(ci.pathPrefix), "%s", prefix.c_str());
                const XrResult r = xr.pfnCaptureAtlas(xr.session, &ci, nullptr);
                LOG_INFO("Atlas capture requested (I): %s -> %s%s",
                         XR_SUCCEEDED(r) ? "OK" : "failed", ci.pathPrefix,
                         "_atlas_<views>_<cols>x<rows>.png");
                break;
            }

            case XK_v: case XK_V: {
                if (xr.renderingModeCount == 0) {
                    LOG_INFO("V: the runtime enumerated no rendering modes");
                    break;
                }
                const uint32_t cur = (xr.activeRenderingMode < xr.renderingModeCount)
                                         ? xr.activeRenderingMode : 0u;
                RequestRenderingMode(xr, (cur + 1u) % xr.renderingModeCount, "V");
                break;
            }

            case XK_0: case XK_1: case XK_2: case XK_3: case XK_4:
            case XK_5: case XK_6: case XK_7: case XK_8: {
                const uint32_t want = (uint32_t)(sym - XK_0);
                if (want >= xr.renderingModeCount) {
                    LOG_INFO("Mode %u: the runtime offers only %u mode(s)", want, xr.renderingModeCount);
                    break;
                }
                RequestRenderingMode(xr, want, "0-8");
                break;
            }

            // Dynamic-recenter pins: P arms, then X/Y/Z toggle that axis' pin.
            // onKey consumes P (arm) and X/Y/Z (while armed).
            case XK_p: case XK_P:
            case XK_x: case XK_X:
            case XK_y: case XK_Y:
            case XK_z: case XK_Z: {
                char c = (char)sym;
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if (g_recenter.onKey(c)) {
                    char lbl[24];
                    g_recenter.hudLabel(lbl, sizeof(lbl));
                    LOG_INFO("Recenter: %s", lbl);
                }
                break;
            }

            default: break;
            }
            break;
        }

        case KeyRelease: {
            // XkbSetDetectableAutoRepeat is on, so a KeyRelease is a real
            // release, not the front half of an auto-repeat.
            const KeySym sym = XLookupKeysym(&ev.xkey, 0);
            switch (sym) {
            case XK_w: case XK_W: g_keyW = false; break;
            case XK_a: case XK_A: g_keyA = false; break;
            case XK_s: case XK_S: g_keyS = false; break;
            case XK_d: case XK_D: g_keyD = false; break;
            case XK_q: case XK_Q: g_keyQ = false; break;
            case XK_e: case XK_E: g_keyE = false; break;
            default: break;
            }
            break;
        }

        case ButtonPress: {
            switch (ev.xbutton.button) {
            case Button1: {
                // Double-click = focus. A single left press is deliberately
                // inert: the avatar faces the viewer via the yaw billboard, so
                // a drag must not rotate it — windows/main.cpp pins the
                // drag-accumulated yaw and pitch to 0 for the same reason.
                const unsigned long dt = (unsigned long)(ev.xbutton.time - s_lastClickTime);
                const bool near = abs(ev.xbutton.x - s_lastClickX) <= kDoubleClickSlopPx &&
                                  abs(ev.xbutton.y - s_lastClickY) <= kDoubleClickSlopPx;
                if (s_lastClickTime != 0 && dt <= kDoubleClickMs && near) {
                    PickFocus(ev.xbutton.x, ev.xbutton.y);
                    s_lastClickTime = 0;   // a triple-click is not two doubles
                } else {
                    s_lastClickTime = ev.xbutton.time;
                    s_lastClickX = ev.xbutton.x;
                    s_lastClickY = ev.xbutton.y;
                }
                break;
            }
            case Button3: {
                // Right-drag moves the borderless overlay through the
                // client-owned, phase-snapped drag (see WeaveSnap above).
                // Decorated windows have a title bar for that (a WM-owned,
                // unsnapped move — the deliberate B escape hatch), so the
                // gesture only starts while undecorated, matching the `active`
                // gate on Windows. Never in fullscreen: a stray gesture would
                // slide the panel-sized weave off the panel.
                if (g_decorated || g_fullscreen) break;
                BeginWindowDrag(xr, ev.xbutton.x_root, ev.xbutton.y_root);
                break;
            }
            case Button4:   // wheel up
            case Button5: { // wheel down
                const float factor = (ev.xbutton.button == Button4) ? 1.1f : (1.0f / 1.1f);
                if ((ev.xbutton.state & ShiftMask) != 0) {
                    // Shift+wheel drives the 3D-effect strength, the same
                    // single knob the shared Windows handler exposes.
                    g_ipdFactor *= factor;
                    if (g_ipdFactor < 0.0f) g_ipdFactor = 0.0f;
                    if (g_ipdFactor > 1.0f) g_ipdFactor = 1.0f;
                    LOG_INFO("3D strength: %.2f (Shift+wheel)", g_ipdFactor);
                } else {
                    g_zoomFactor *= factor;
                    if (g_zoomFactor < 0.1f) g_zoomFactor = 0.1f;
                    if (g_zoomFactor > 10.0f) g_zoomFactor = 10.0f;
                }
                break;
            }
            default: break;
            }
            break;
        }

        case ButtonRelease:
            if (ev.xbutton.button == Button3) {
                // Apply the final coalesced position before ending, so the
                // window lands where the button came up, not one sample short.
                if (haveDragMotion && g_dragging) {
                    MoveWindowSnapped(xr, g_dragOriginX + (dragMotionRootX - g_dragPtrX),
                                      g_dragOriginY + (dragMotionRootY - g_dragPtrY));
                    haveDragMotion = false;
                }
                EndWindowDrag(xr, "release");
            }
            break;

        case MotionNotify:
            if (g_dragging) {
                haveDragMotion = true;
                dragMotionRootX = ev.xmotion.x_root;
                dragMotionRootY = ev.xmotion.y_root;
            }
            break;

        case FocusOut:
            // A grab (a WM keybinding overlay, another client grabbing the
            // keyboard) reports focus out and straight back in; only a real
            // focus change should drop the keys.
            if (ev.xfocus.mode == NotifyGrab || ev.xfocus.mode == NotifyUngrab) break;
            // Losing KEYBOARD focus means losing the keys: a held W released
            // over another window never reaches us, and the avatar would pan
            // forever. Deliberately FocusOut and not LeaveNotify — the XShape
            // input region is rebuilt every frame as the avatar animates, so
            // the pointer crosses its boundary constantly while the keyboard
            // focus is perfectly stable, and clearing on a pointer crossing
            // would make WASD unusable whenever the cursor sat near an edge.
            g_keyW = g_keyA = g_keyS = g_keyD = g_keyQ = g_keyE = false;
            haveDragMotion = false;
            EndWindowDrag(xr, "focus lost");
            break;

        case ConfigureNotify:
            if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
                xr.xWinW = (unsigned int)ev.xconfigure.width;
                xr.xWinH = (unsigned int)ev.xconfigure.height;
                g_clientPxW = xr.xWinW;
                g_clientPxH = xr.xWinH;
                // Only the rect OFFSET is snapped, but hand the DP the real
                // size so a future size-aware snap is not fed a stale one.
                g_weaveSnap.setExtent(xr.xWinW, xr.xWinH);
            }
            break;
        default: break;
        }
    }

    if (haveDragMotion && g_dragging) {
        // Absolute, not incremental: origin + (pointer now - pointer at grab).
        // A snap that holds the window back a few pixels therefore never makes
        // the window lag the pointer permanently.
        MoveWindowSnapped(xr, g_dragOriginX + (dragMotionRootX - g_dragPtrX),
                          g_dragOriginY + (dragMotionRootY - g_dragPtrY));
    }
}

// ── B / F11 ────────────────────────────────────────────────────────────────
// The decoration toggle also drops the XShape input region (ClickthroughUpdate
// clears it while decorated), so the whole framed window becomes interactive
// for move/resize — the same trade windows/main.cpp makes with
// SetWindowRgn(NULL).
static void ToggleDecoration(AppXrSession& xr) {
    if (xr.xDisplay == nullptr || xr.xWindow == 0) return;
    if (g_fullscreen) return;   // decoration is meaningless in fullscreen
    EndWindowDrag(xr, "decoration toggled");   // a WM frame now owns the move
    g_decorated = !g_decorated;
    SetMotifDecorations(xr.xDisplay, xr.xWindow, g_decorated);
    XFlush(xr.xDisplay);
    LOG_INFO("Window decoration: %s (B)", g_decorated ? "ON (move/resize)" : "OFF (borderless)");
}

static void ToggleFullscreen(AppXrSession& xr) {
    if (xr.xDisplay == nullptr || xr.xWindow == 0) return;
    EndWindowDrag(xr, "fullscreen toggled");   // a fullscreen window is never dragged
    g_fullscreen = !g_fullscreen;
    SetNetWmState(xr.xDisplay, xr.xWindow, "_NET_WM_STATE_FULLSCREEN", g_fullscreen);
    LOG_INFO("%s fullscreen mode (F11)", g_fullscreen ? "Entered" : "Exited");
}

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    bool hasXlibBinding = false;
    bool hasDisplayInfo = false;
    bool hasViewRig = false;
    bool hasLocal3DZone = false;
    bool hasDisplayZones = false;
    bool hasAtlasCapture = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0) hasXlibBinding = true;
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) hasDisplayInfo = true;
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) hasViewRig = true;
        if (strcmp(ext.extensionName, XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME) == 0) hasLocal3DZone = true;
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_ZONES_EXTENSION_NAME) == 0) hasDisplayZones = true;
        if (strcmp(ext.extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0) hasAtlasCapture = true;
        if (strcmp(ext.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0) xr.hasWeave = true;
        if (strcmp(ext.extensionName, XR_DXR_DEPTH_BUDGET_EXTENSION_NAME) == 0) {
            xr.hasDepthBudget = true;
            // The RUNTIME's version, not this app's vendored SPEC_VERSION — the
            // v3 content-mask chain must never reach a v2 runtime.
            xr.depthBudgetVersion = ext.extensionVersion;
        }
    }

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }
    LOG_INFO("XR_DXR_xlib_window_binding: %s", hasXlibBinding ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_display_info: %s", hasDisplayInfo ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_view_rig: %s", hasViewRig ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_local_3d_zone: %s", hasLocal3DZone ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_display_zones: %s", hasDisplayZones ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_atlas_capture: %s", hasAtlasCapture ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_depth_budget: %s (v%u)", xr.hasDepthBudget ? "AVAILABLE" : "NOT FOUND",
             xr.depthBudgetVersion);
    LOG_INFO("XR_DXR_weave (drag phase snap): %s", xr.hasWeave ? "AVAILABLE" : "NOT FOUND");

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    // Enable the app-owned-window binding when the runtime exposes it — required
    // to hand over our transparent X11 window (else we run hosted-NULL).
    if (hasXlibBinding) {
        enabledExtensions.push_back(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
        xr.hasXlibBinding = true;
    }
    // Enable XR_DXR_display_info — the switch that makes this an EXTENSION app:
    // the runtime tiles window×recommendedViewScale (window-relative Kooima)
    // instead of the legacy 0.50×1.00 compromise, and we query the panel dims +
    // per-view scale + desktop position below. Same enable/query as
    // cube_handle_vk_linux.
    if (hasDisplayInfo) {
        enabledExtensions.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
        xr.hasDisplayInfo = true;
    }
    // Enable the display view rig so the runtime returns render-ready view
    // poses/FOV (camera at the eye) instead of us hand-rolling the Kooima math.
    // The rig needs display_info to do the window-relative Kooima, so gate the
    // enable on both — exactly like cube_handle_vk_linux ("view_rig needs
    // display_info"). Falls back to raw xrLocateViews fov if either is absent.
    if (hasViewRig && hasDisplayInfo) {
        enabledExtensions.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
        xr.hasViewRig = true;
    } else if (hasViewRig) {
        LOG_INFO("XR_DXR_view_rig present but XR_DXR_display_info is not — "
                 "not enabling the rig (needs display_info for window-relative Kooima)");
    }
    // Enable the Local2D layer so the runtime composites the flat 2D speech
    // bubble in the top 25% band (the runtime's software-composite path handles
    // Local2D on Linux with no platform guard). Guarded everywhere on
    // g_hasLocal3DZone so the app still runs if the runtime lacks it.
    if (hasLocal3DZone) {
        enabledExtensions.push_back(XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME);
        g_hasLocal3DZone = true;
    }
    // Enable XR_DXR_display_zones (the tiger-zone). It composes view_rig +
    // local_3d_zone, so require both prerequisites (view_rig is enabled above
    // only when display_info is also present). Without them the app degrades to
    // the full-tile + Local2D path. Same gate as cube_zones_vk_linux.
    if (hasDisplayZones && (!hasLocal3DZone || !xr.hasViewRig)) {
        LOG_WARN("XR_DXR_display_zones advertised without its prerequisites "
                 "(local_3d_zone / view_rig+display_info) — tiger-zone disabled");
        hasDisplayZones = false;
    }
    if (hasDisplayZones) {
        enabledExtensions.push_back(XR_DXR_DISPLAY_ZONES_EXTENSION_NAME);
        g_hasDisplayZones = true;
    }
    // The I key. Harmless when absent — the handler says so and does nothing.
    if (hasAtlasCapture) {
        enabledExtensions.push_back(XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME);
        xr.hasAtlasCapture = true;
    }
    // XR_DXR_depth_budget (ADR-040): opt in so the runtime publishes its
    // advisory rear budget on XrViewState. Without the opt-in the runtime keeps
    // the conservative default and dxr::ResolveClipPlanes falls back to the
    // hard ZDP clip — i.e. exactly the pre-extension behaviour, which is what
    // this leg did before.
    if (xr.hasDepthBudget) {
        enabledExtensions.push_back(XR_DXR_DEPTH_BUDGET_EXTENSION_NAME);
    }
    // XR_DXR_weave: enabled only for xrWeaveSnapWindowRectDXR, the snap the
    // client-owned RMB drag routes every step through. Purely optional — absent,
    // the drag runs unsnapped rather than failing.
    if (xr.hasWeave) {
        enabledExtensions.push_back(XR_DXR_WEAVE_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "AvatarHandleVkLinux",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "None",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();
    XR_CHECK(xrCreateInstance(&createInfo, &xr.instance));

    // Resolve the XR_DXR_display_zones entry points (caps + per-zone view size).
    // If either is unresolved the tiger-zone is disabled → full-tile fallback.
    if (g_hasDisplayZones) {
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneCapabilitiesDXR",
            (PFN_xrVoidFunction*)&g_pfnGetZoneCaps);
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneRecommendedViewSizeDXR",
            (PFN_xrVoidFunction*)&g_pfnGetZoneViewSize);
        if (g_pfnGetZoneCaps == nullptr || g_pfnGetZoneViewSize == nullptr) {
            LOG_WARN("XR_DXR_display_zones entry points unresolved — tiger-zone disabled");
            g_hasDisplayZones = false;
        }
    }

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

    // #1486/#1500 — pick the view configuration right after xrGetSystem and
    // BEFORE the first typed call (xrEnumerateViewConfigurationViews below,
    // xrBeginSession's primaryViewConfigurationType, both xrLocateViews sites).
    //
    // This leg is stereo-FIXED today (it enumerates no rendering modes and
    // renders a fixed 2-tile atlas), so PRIMARY_STEREO would also work — the
    // runtime's permissive rule accepts a 2-view layer under either type. It
    // opts in anyway for two reasons: every leg of this demo then begins the
    // same way, which is one less per-platform difference to remember; and the
    // submit clamp below becomes meaningful the day this leg grows mode
    // switching, instead of needing the opt-in retro-fitted with it. The helper
    // degrades to PRIMARY_STEREO on a runtime that does not enumerate the DXR
    // type.
    xr.viewConfigType = DxrSelectViewConfigType(xr.instance, xr.systemId);
    LOG_INFO("View configuration type: %s", DxrViewConfigTypeName(xr.viewConfigType));

    // Query XR_DXR_display_info: physical panel dims (m2v anchor), pixel dims,
    // per-view recommended scale (window×scale tiling), and the 3D-panel desktop
    // position (INV-1.3). The runtime fills the chained structs only when
    // XR_DXR_display_info is enabled; the zero-init defaults are the safe
    // fallback. Same chain as cube_handle_vk_linux.
    if (xr.hasDisplayInfo) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR displayInfo = {XR_TYPE_DISPLAY_INFO_DXR};
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        displayInfo.next = &desktopPos;
        sysProps.next = &displayInfo;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            xr.displayScreenLeft = desktopPos.left;
            xr.displayScreenTop = desktopPos.top;
            xr.displayWidthM = displayInfo.displaySizeMeters.width;
            xr.displayHeightM = displayInfo.displaySizeMeters.height;
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            if (displayInfo.recommendedViewScaleX > 0.0f) xr.viewScaleX = displayInfo.recommendedViewScaleX;
            if (displayInfo.recommendedViewScaleY > 0.0f) xr.viewScaleY = displayInfo.recommendedViewScaleY;
            LOG_INFO("Display info: desktop (%d,%d), %.4f x %.4f m, %ux%u px, view scale %.3fx%.3f",
                     xr.displayScreenLeft, xr.displayScreenTop,
                     xr.displayWidthM, xr.displayHeightM,
                     xr.displayPixelWidth, xr.displayPixelHeight,
                     xr.viewScaleX, xr.viewScaleY);
        } else {
            LOG_WARN("xrGetSystemProperties(display_info) failed — falling back to per-view recommended dims");
        }
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));
    xr.viewWidth = xr.configViews[0].recommendedImageRectWidth;
    xr.viewHeight = xr.configViews[0].recommendedImageRectHeight;
    LOG_INFO("View config: %u views, per-view %ux%u", viewCount, xr.viewWidth, xr.viewHeight);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfn));
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(xr.instance, xr.systemId, &req));
    return true;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&pfn));
    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extStr.data());

    std::vector<std::string> names;
    { size_t start = 0;
      while (start < extStr.size()) {
          size_t end = extStr.find(' ', start);
          if (end == std::string::npos) end = extStr.size();
          std::string n = extStr.substr(start, end - start);
          if (!n.empty() && n[0] != '\0') names.push_back(n);
          start = end + 1;
      } }

    std::vector<const char*> ptrs;
    for (auto& n : names) ptrs.push_back(n.c_str());

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "AvatarHandleVkLinux";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)ptrs.size();
    createInfo.ppEnabledExtensionNames = ptrs.data();
    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    PFN_xrGetVulkanGraphicsDeviceKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&pfn));
    XR_CHECK(pfn(xr.instance, xr.systemId, vkInstance, &physDevice));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    return true;
}

static bool DeviceSupportsExtension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, props.data());
    for (auto& p : props) if (strcmp(p.extensionName, name) == 0) return true;
    return false;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, VkPhysicalDevice physDevice,
                                      std::vector<const char*>& deviceExtensions,
                                      std::vector<std::string>& storage) {
    PFN_xrGetVulkanDeviceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&pfn));
    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extStr.data());

    storage.clear();
    deviceExtensions.clear();
    { size_t start = 0;
      while (start < extStr.size()) {
          size_t end = extStr.find(' ', start);
          if (end == std::string::npos) end = extStr.size();
          std::string n = extStr.substr(start, end - start);
          if (!n.empty() && n[0] != '\0') storage.push_back(n);
          start = end + 1;
      } }

    // runtime#757 / leia-plugin#81 — the transparency desktop-capture producer in the
    // Leia DP imports the live desktop as a dma-buf VkImage, which needs these
    // device extensions ENABLED on the app-owned VkDevice. We're an
    // XR_KHR_vulkan_enable (enable1) app, so WE own vkCreateDevice — the runtime
    // advertises only its own required set via xrGetVulkanDeviceExtensionsKHR, so
    // append the capture extensions here. Support-checked: a device/driver
    // lacking them just skips, and the producer then declines gracefully (opaque
    // / 2D-under-backdrop) rather than failing device creation. Must append to
    // `storage` BEFORE building the const char* list so the pointers stay valid.
    const char* kCaptureExts[] = {
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, // usually already in the runtime set
    };
    for (const char* e : kCaptureExts) {
        bool already = false;
        for (auto& s : storage) if (s == e) { already = true; break; }
        if (already) continue;
        if (DeviceSupportsExtension(physDevice, e)) {
            storage.push_back(e);
            LOG_INFO("transparency: enabling capture device ext %s", e);
        } else {
            LOG_WARN("transparency: device lacks %s — desktop capture will decline", e);
        }
    }

    for (auto& n : storage) deviceExtensions.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, families.data());
    for (uint32_t i = 0; i < count; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueFamilyIndex = i; return true; }
    LOG_ERROR("No graphics queue family found");
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
                               const std::vector<const char*>& deviceExtensions,
                               VkDevice& device, VkQueue& graphicsQueue) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();
    VK_CHECK(vkCreateDevice(physDevice, &createInfo, nullptr, &device));
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    return true;
}

/*!
 * (Re-)read the runtime's rendering-mode table.
 *
 * Called once after xrCreateSession to ADOPT whatever mode the panel is already
 * in (this leg never forces one at startup), and again after every accepted V /
 * 0-8 request and every XrEventDataRenderingModeChangedDXR, so the submitted
 * view count and the `isRequestable` gate follow the runtime rather than a
 * cached guess.
 */
static void RefreshRenderingModes(AppXrSession& xr, bool logTable) {
    if (xr.pfnEnumerateRenderingModes == nullptr || xr.session == XR_NULL_HANDLE) return;
    uint32_t modeCount = 0;
    if (XR_FAILED(xr.pfnEnumerateRenderingModes(xr.session, 0, &modeCount, nullptr)) ||
        modeCount == 0) {
        return;
    }
    std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
    for (uint32_t i = 0; i < modeCount; i++) {
        modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
        modes[i].next = nullptr;
    }
    if (XR_FAILED(xr.pfnEnumerateRenderingModes(xr.session, modeCount, &modeCount, modes.data()))) {
        return;
    }
    xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
    xr.activeRenderingMode = UINT32_MAX;
    if (logTable) LOG_INFO("Display rendering modes (%u):", modeCount);
    for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
        xr.renderingModeViewCounts[i] = modes[i].viewCount;
        xr.renderingModeIsRequestable[i] = (modes[i].isRequestable == XR_TRUE);
        if (modes[i].isActive == XR_TRUE) xr.activeRenderingMode = i;
        if (logTable) {
            LOG_INFO("  [%u] %s (views=%u, tiles=%ux%u, 3D=%d%s%s)",
                     modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                     modes[i].tileColumns ? modes[i].tileColumns : 1u,
                     modes[i].tileRows ? modes[i].tileRows : 1u,
                     modes[i].hardwareDisplay3D,
                     modes[i].isActive == XR_TRUE ? ", ACTIVE" : "",
                     modes[i].isRequestable == XR_TRUE ? "" : ", LOCKED");
        }
    }
}

//! V / 0-8. `isRequestable` is false for a non-controller session under a
//! workspace, where the controller is the sole mode authority and the runtime
//! drops app requests — say so rather than looking inert.
static void RequestRenderingMode(AppXrSession& xr, uint32_t modeIndex, const char* via) {
    if (xr.pfnRequestRenderingMode == nullptr) {
        LOG_INFO("%s: xrRequestDisplayRenderingModeDXR unavailable", via);
        return;
    }
    if (modeIndex < xr.renderingModeCount && !xr.renderingModeIsRequestable[modeIndex]) {
        LOG_INFO("%s: mode %u is locked by the workspace controller", via, modeIndex);
        return;
    }
    const XrResult r = xr.pfnRequestRenderingMode(xr.session, modeIndex);
    LOG_INFO("Rendering mode -> %u (%s, via %s)", modeIndex,
             XR_SUCCEEDED(r) ? "OK" : "rejected", via);
    // The runtime also pushes XrEventDataRenderingModeChangedDXR, but PollEvents
    // may not run before the next render; re-read now so the submitted view
    // count follows immediately.
    if (XR_SUCCEEDED(r)) RefreshRenderingModes(xr, /*logTable=*/false);
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
                          VkDevice device, uint32_t queueFamilyIndex) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = 0;

    // App-owned-window path: chain the xlib binding (→ vkBinding) so the runtime
    // renders into our X11 window — this is what makes the avatar a proper HANDLE
    // app (win/mac parity), so the compositor tiles window×scaleXY (window-relative
    // Kooima) instead of the display-scoped hosted-NULL path. Falls back to
    // hosted-NULL when no window / the extension is absent (e.g. headless CI),
    // where the graphics binding is chained straight in and the runtime
    // self-creates its window.
    //
    // Transparency (transparentBackgroundEnabled) is the avatar's raison
    // d'être — the compose-through-to-desktop overlay (runtime#757 / #758) —
    // and is ON by default, matching Windows/macOS/Android. The shipped Linux
    // runtime supports the transparent XCB surface since runtime#758 (v2.1.2+)
    // and live desktop weave-under (portal/PipeWire bg-capture) since the
    // dma-buf device-extension enable (runtime v2.2.0 + leia-sr v2.0.5).
    // AVATAR_TRANSPARENT=0 opts OUT (debug escape hatch, e.g. an old runtime
    // whose compositor aborts on the transparent surface).
    const bool wantTransparent = []() {
        const char* e = getenv("AVATAR_TRANSPARENT");
        return e == nullptr || e[0] == '\0' || e[0] != '0';
    }();
    // Seed the Ctrl+T state from the same flag: the session's transparency is
    // fixed here, and drawing a transparent frame into an OPAQUE session gives
    // black where the desktop should be, not see-through.
    g_transparentBg = wantTransparent;
    XrXlibWindowBindingCreateInfoDXR xlibBinding = {XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR};
    xlibBinding.next = &vkBinding;
    xlibBinding.xDisplay = xr.xDisplay;
    xlibBinding.window = xr.xWindow;
    xlibBinding.transparentBackgroundEnabled = wantTransparent ? XR_TRUE : XR_FALSE;

    const bool useAppWindow = xr.hasXlibBinding && xr.xDisplay != nullptr && xr.xWindow != 0;
    xr.usingAppWindow = useAppWindow;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = useAppWindow ? (const void*)&xlibBinding : (const void*)&vkBinding;
    sessionInfo.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created (%s)",
             useAppWindow ? (wantTransparent ? "app-owned window, transparent overlay (default)"
                                             : "app-owned window, opaque handle app (AVATAR_TRANSPARENT=0)")
                          : "hosted-NULL: runtime self-creates the window");

    // Drag-time window-origin phase snap (runtime#1588), used by the
    // client-owned RMB drag. Resolved defensively: identity when the runtime
    // does not serve it, and then the drag is merely unsnapped.
    if (useAppWindow) {
        g_weaveSnap.attach(xr.hasWeave ? xr.instance : XR_NULL_HANDLE, xr.session,
                           xr.xWinW, xr.xWinH);
        LOG_INFO("xrWeaveSnapWindowRectDXR: %s — a window drag %s",
                 g_weaveSnap.available() ? "RESOLVED" : "unavailable on this runtime",
                 g_weaveSnap.available() ? "will be phase-snapped by the display processor"
                                         : "lands on the raw pointer position (unsnapped)");
    }

    // ADOPT the runtime's active rendering mode (display_info v13). This leg has
    // no mode-switch UI and never REQUESTS a mode — it only reads which one the
    // panel is already in, so the submitted view count follows the runtime
    // instead of the hardcoded 2 it used to assume. That matters as soon as the
    // panel boots into a 1-view mode (SIM_DISPLAY_OUTPUT=2d): the app used to
    // submit two views into a one-tile atlas.
    if (xr.hasDisplayInfo) {
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
                              (PFN_xrVoidFunction*)&xr.pfnEnumerateRenderingModes);
    }
    RefreshRenderingModes(xr, /*logTable=*/true);
    if (xr.activeRenderingMode != UINT32_MAX && xr.activeRenderingMode < xr.renderingModeCount) {
        LOG_INFO("Adopting runtime's active rendering mode: %u (views=%u)",
                 xr.activeRenderingMode, xr.renderingModeViewCounts[xr.activeRenderingMode]);
    } else {
        LOG_INFO("Runtime named no active rendering mode — assuming the 2-view default");
    }

    // V / 0-8 (mode switching) and T (eye-tracking mode) both live behind
    // XR_DXR_display_info. Unresolved is not fatal: the key handlers say so.
    if (xr.hasDisplayInfo) {
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR",
                              (PFN_xrVoidFunction*)&xr.pfnRequestRenderingMode);
        xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeDXR",
                              (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingMode);
    }
    if (xr.hasAtlasCapture) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasDXR",
                              (PFN_xrVoidFunction*)&xr.pfnCaptureAtlas);
        if (xr.pfnCaptureAtlas == nullptr) {
            LOG_WARN("xrCaptureAtlasDXR unresolved — the I key is inert");
            xr.hasAtlasCapture = false;
        }
    }
    return true;
}

/*!
 * Views the ACTIVE rendering mode wants, or 2 when the runtime named no mode.
 *
 * This leg renders a FIXED 2-tile horizontal atlas, so it can honour a 1-view
 * mode (submit one tile) but not a 4-view one — the caller's
 * DxrClampSubmitViewCount() cuts the wanted count down to the 2 tiles that
 * exist and logs the disagreement once, which is the honest outcome until the
 * tile layout is generalised to the mode's cols x rows grid.
 */
static uint32_t ActiveModeViewCount(const AppXrSession& xr) {
    if (xr.activeRenderingMode != UINT32_MAX &&
        xr.activeRenderingMode < xr.renderingModeCount &&
        xr.renderingModeViewCounts[xr.activeRenderingMode] > 0) {
        return xr.renderingModeViewCounts[xr.activeRenderingMode];
    }
    return 2u;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo localInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &localInfo, &xr.localSpace));

    XrReferenceSpaceCreateInfo viewInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &viewInfo, &xr.viewSpace));
    return true;
}

static bool CreateSwapchain(AppXrSession& xr) {
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));
    // Prefer a UNORM swapchain (matches the macOS avatar). ModelRenderer draws to
    // an internal UNORM target and its shader gamma-encodes explicitly; an sRGB
    // swapchain instead relies on the blit to sRGB-encode, which it doesn't — so
    // colors come out washed-out / desaturated (Suki's DS1 report). Pick UNORM if
    // offered, else sRGB, else the R8G8B8A8_UNORM fallback.
    int64_t selectedFormat = formats.empty() ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : formats[0];
    for (int64_t f : formats) {
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) { selectedFormat = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) selectedFormat = f;
    }

    const auto& view = xr.configViews[0];
    uint32_t scWidth = view.recommendedImageRectWidth * 2; // stereo SBS atlas
    uint32_t scHeight = view.recommendedImageRectHeight;

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = scWidth;
    swapchainInfo.height = scHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchain.swapchain));

    xr.swapchain.format = selectedFormat;
    xr.swapchain.width = scWidth;
    xr.swapchain.height = scHeight;
    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imageCount, nullptr));
    xr.swapchain.imageCount = imageCount;
    LOG_INFO("Atlas swapchain: %ux%u, %u images", scWidth, scHeight, imageCount);
    return true;
}

static bool PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = e->state;
            switch (xr.sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = xr.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(xr.session, &beginInfo))) {
                    xr.sessionRunning = true;
                    LOG_INFO("Session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                xr.exitRequested = true;
                break;
            default: break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            xr.exitRequested = true;
            break;
        case (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR: {
            // Someone changed the mode — us via V / 0-8, or the workspace
            // controller. Re-read the table so the submitted view count and the
            // isRequestable gate follow rather than going stale.
            auto* e = (XrEventDataRenderingModeChangedDXR*)&event;
            LOG_INFO("Rendering mode changed: %u -> %u", e->previousModeIndex, e->currentModeIndex);
            RefreshRenderingModes(xr, /*logTable=*/false);
            break;
        }
        default: break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.viewSpace != XR_NULL_HANDLE) xrDestroySpace(xr.viewSpace);
    if (xr.localSpace != XR_NULL_HANDLE) xrDestroySpace(xr.localSpace);
    if (xr.session != XR_NULL_HANDLE) xrDestroySession(xr.session);
    if (xr.instance != XR_NULL_HANDLE) xrDestroyInstance(xr.instance);
    // Tear down the app-owned X11 window after the runtime has released it.
    if (xr.xWindow != 0 && xr.xDisplay != nullptr) XDestroyWindow(xr.xDisplay, xr.xWindow);
    if (xr.xColormap != 0 && xr.xDisplay != nullptr) XFreeColormap(xr.xDisplay, xr.xColormap);
    if (xr.xDisplay != nullptr) XCloseDisplay(xr.xDisplay);
    xr.xWindow = 0;
    xr.xColormap = 0;
    xr.xDisplay = nullptr;
}

static void SignalHandler(int) { g_running = false; }

// ============================================================================
// Speech-bubble swapchain + CPU rasterizer (stb_truetype)
// ============================================================================

static bool CreateBubbleSwapchain(AppXrSession& xr, VkDevice dev, VkPhysicalDevice phys) {
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fc, nullptr);
    std::vector<int64_t> fmts(fc);
    if (fc) xrEnumerateSwapchainFormats(xr.session, fc, &fc, fmts.data());
    // Prefer UNORM RGBA8 so the CPU-drawn sRGB bytes pass through with no hidden
    // sRGB decode (same reason the atlas swapchain uses UNORM).
    int64_t fmt = fmts.empty() ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : fmts[0];
    for (int64_t f : fmts) {
        if (f == VK_FORMAT_R8G8B8A8_UNORM) { fmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM) fmt = f;
    }
    g_bubbleFormat = fmt;

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1;
    ci.width = kBubbleTexW; ci.height = kBubbleTexH;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &ci, &g_bubbleSwapchain))) {
        LOG_WARN("Bubble swapchain create failed");
        return false;
    }
    uint32_t ic = 0;
    xrEnumerateSwapchainImages(g_bubbleSwapchain, 0, &ic, nullptr);
    g_bubbleImages.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g_bubbleSwapchain, ic, &ic,
        (XrSwapchainImageBaseHeader*)g_bubbleImages.data());

    g_bubbleStaging = modelCreateBuffer(dev, phys, (VkDeviceSize)kBubbleTexW * kBubbleTexH * 4,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (g_bubbleStaging.buffer == VK_NULL_HANDLE) return false;
    if (vkMapMemory(dev, g_bubbleStaging.memory, 0, (VkDeviceSize)kBubbleTexW * kBubbleTexH * 4,
                    0, &g_bubbleStagingMapped) != VK_SUCCESS || !g_bubbleStagingMapped)
        return false;
    LOG_INFO("Bubble swapchain ready (%ux%u, %u images, format=%lld)",
             kBubbleTexW, kBubbleTexH, ic, (long long)fmt);
    return true;
}

// Load the first available system TTF (once). No CoreText/DirectWrite on Linux.
static stbtt_fontinfo g_bubbleFont;
static std::vector<unsigned char> g_bubbleFontData;
static bool g_bubbleFontLoaded = false;

static void LoadBubbleFont() {
    static bool s_tried = false;
    if (s_tried) return;
    s_tried = true;
    const char* kFontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };
    for (const char* p : kFontPaths) {
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0) { fclose(f); continue; }
        g_bubbleFontData.resize((size_t)len);
        bool ok = fread(g_bubbleFontData.data(), 1, (size_t)len, f) == (size_t)len;
        fclose(f);
        if (!ok) continue;
        if (stbtt_InitFont(&g_bubbleFont, g_bubbleFontData.data(),
                           stbtt_GetFontOffsetForIndex(g_bubbleFontData.data(), 0))) {
            g_bubbleFontLoaded = true;
            LOG_INFO("Bubble font: %s", p);
            return;
        }
    }
    LOG_WARN("Bubble: no system TTF found (tried DejaVu / Liberation) — drawing the "
             "panel without text");
}

// Straight-alpha source width of an ASCII string at a given stbtt scale.
static float BubbleTextWidth(float scale, const char* s) {
    float w = 0.0f;
    for (const char* p = s; *p; ++p) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_bubbleFont, (unsigned char)*p, &adv, &lsb);
        w += adv * scale;
        if (p[1]) w += stbtt_GetCodepointKernAdvance(&g_bubbleFont, (unsigned char)*p,
                                                     (unsigned char)p[1]) * scale;
    }
    return w;
}

// Greedy word-wrap `text` to `maxW` at `scale`; returns the wrapped lines.
static std::vector<std::string> BubbleWrap(const char* text, float scale, float maxW) {
    std::vector<std::string> lines;
    std::string cur, word;
    for (const char* p = text;; ++p) {
        if (*p != ' ' && *p != '\0') { word.push_back(*p); continue; }
        if (!word.empty()) {
            std::string trial = cur.empty() ? word : cur + " " + word;
            if (cur.empty() || BubbleTextWidth(scale, trial.c_str()) <= maxW) {
                cur.swap(trial);
            } else {
                lines.push_back(cur);
                cur = word;
            }
            word.clear();
        }
        if (*p == '\0') break;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Premultiplied source-over of a straight-alpha source (sr,sg,sb in [0,1],
// sa in [0,1]) into an RGBA8 destination texel.
static inline void BubbleBlend(unsigned char* d, float sr, float sg, float sb, float sa) {
    if (sa <= 0.0f) return;
    const float dr = d[0] / 255.0f, dg = d[1] / 255.0f, db = d[2] / 255.0f, da = d[3] / 255.0f;
    const float ia = 1.0f - sa;
    // Source premultiplied = s*sa; source-over in premultiplied space.
    const float orr = sr * sa + dr * ia;
    const float ogg = sg * sa + dg * ia;
    const float obb = sb * sa + db * ia;
    const float oaa = sa + da * ia;
    d[0] = (unsigned char)(orr * 255.0f + 0.5f);
    d[1] = (unsigned char)(ogg * 255.0f + 0.5f);
    d[2] = (unsigned char)(obb * 255.0f + 0.5f);
    d[3] = (unsigned char)(oaa * 255.0f + 0.5f);
}

// Signed distance to a rounded rect (centered box); negative inside.
static float BubbleRoundedRectDist(float px, float py, float cx, float cy,
                                   float hw, float hh, float r) {
    const float qx = std::fabs(px - cx) - (hw - r);
    const float qy = std::fabs(py - cy) - (hh - r);
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    const float outside = std::sqrt(ax * ax + ay * ay);
    const float mx = qx > qy ? qx : qy;
    const float inside = mx < 0.0f ? mx : 0.0f;
    return outside + inside - r;
}

// Draw the rounded glassy pill + centred, word-wrapped, size-fitted greeting
// into the top-left subW×subH of a kBubbleTexW×kBubbleTexH RGBA8 buffer
// (PREMULTIPLIED, row 0 = top); the rest is left transparent. Returns the buffer
// (static, cached by size). Replaces macos/main.mm RenderBubbleBitmap (CoreText).
static const uint8_t* RenderBubbleBitmap(uint32_t subW, uint32_t subH) {
    static std::vector<uint8_t> buf;
    static uint32_t s_lastW = 0, s_lastH = 0;
    // The greeting is static — only redraw when the band sub-rect changes size.
    if (!buf.empty() && subW == s_lastW && subH == s_lastH) return buf.data();
    s_lastW = subW; s_lastH = subH;
    buf.assign((size_t)kBubbleTexW * kBubbleTexH * 4, 0); // fully transparent
    if (subW < 2 || subH < 2) return buf.data();

    LoadBubbleFont();

    auto texel = [&](uint32_t x, uint32_t y) -> unsigned char* {
        return &buf[((size_t)y * kBubbleTexW + x) * 4];
    };

    // Near-edge-to-edge panel (small margin for the rounded corners + desktop to
    // peek through them). Same dark glassy colour as macOS: sRGB(0.05,0.05,0.09),
    // alpha 0.64.
    const float mX = subW * 0.010f, mY = subH * 0.020f;
    const float panelX = mX, panelY = mY;
    const float panelW = (float)subW - 2.0f * mX, panelH = (float)subH - 2.0f * mY;
    const float cx = panelX + panelW * 0.5f, cy = panelY + panelH * 0.5f;
    const float hw = panelW * 0.5f, hh = panelH * 0.5f;
    const float radius = panelH * 0.16f;
    const float kPanelR = 0.05f, kPanelG = 0.05f, kPanelB = 0.09f, kPanelA = 0.64f;

    const uint32_t x1 = subW < kBubbleTexW ? subW : kBubbleTexW;
    const uint32_t y1 = subH < kBubbleTexH ? subH : kBubbleTexH;
    for (uint32_t y = 0; y < y1; ++y) {
        for (uint32_t x = 0; x < x1; ++x) {
            const float d = BubbleRoundedRectDist((float)x + 0.5f, (float)y + 0.5f,
                                                  cx, cy, hw, hh, radius);
            // 1px antialiased coverage at the edge; solid interior.
            const float cov = d < -1.0f ? 1.0f : (d < 0.0f ? -d : 0.0f);
            if (cov <= 0.0f) continue;
            BubbleBlend(texel(x, y), kPanelR, kPanelG, kPanelB, kPanelA * cov);
        }
    }

    // Text (white), centred + word-wrapped + coarse size-fit into the panel
    // interior. If no font opened, the panel ships without text (never crash).
    if (g_bubbleFontLoaded) {
        const float padX = panelW * 0.06f, padY = panelH * 0.16f;
        const float textX = panelX + padX, textY = panelY + padY;
        const float textW = panelW - 2.0f * padX, textH = panelH - 2.0f * padY;
        if (textW > 2.0f && textH > 2.0f) {
            // Largest font (coarse geometric search) whose wrapped greeting fits.
            float bestPx = 8.0f;
            std::vector<std::string> bestLines;
            for (float fs = textH; fs >= 8.0f; fs *= 0.92f) {
                const float scale = stbtt_ScaleForPixelHeight(&g_bubbleFont, fs);
                std::vector<std::string> lines = BubbleWrap(kBubbleText, scale, textW);
                const float lineH = fs * 1.25f;
                const float blockH = lineH * (float)lines.size();
                bool widthOK = true;
                for (const auto& ln : lines)
                    if (BubbleTextWidth(scale, ln.c_str()) > textW) { widthOK = false; break; }
                if (widthOK && blockH <= textH) { bestPx = fs; bestLines.swap(lines); break; }
            }
            if (bestLines.empty())
                bestLines = BubbleWrap(kBubbleText, stbtt_ScaleForPixelHeight(&g_bubbleFont, bestPx), textW);

            const float scale = stbtt_ScaleForPixelHeight(&g_bubbleFont, bestPx);
            int ascent = 0, descent = 0, lineGap = 0;
            stbtt_GetFontVMetrics(&g_bubbleFont, &ascent, &descent, &lineGap);
            const float lineH = bestPx * 1.25f;
            const float blockH = lineH * (float)bestLines.size();
            // Vertically centre the block; first baseline one ascent below its top.
            float lineTop = textY + (textH - blockH) * 0.5f;
            for (const auto& ln : bestLines) {
                const float lw = BubbleTextWidth(scale, ln.c_str());
                float penX = textX + (textW - lw) * 0.5f;
                const float baseline = lineTop + ascent * scale;
                for (const char* p = ln.c_str(); *p; ++p) {
                    int gx0, gy0, gx1, gy1;
                    stbtt_GetCodepointBitmapBox(&g_bubbleFont, (unsigned char)*p, scale, scale,
                                                &gx0, &gy0, &gx1, &gy1);
                    const int gw = gx1 - gx0, gh = gy1 - gy0;
                    if (gw > 0 && gh > 0) {
                        std::vector<unsigned char> gb((size_t)gw * gh);
                        stbtt_MakeCodepointBitmap(&g_bubbleFont, gb.data(), gw, gh, gw,
                                                  scale, scale, (unsigned char)*p);
                        const int dx0 = (int)(penX + 0.5f) + gx0;
                        const int dy0 = (int)(baseline + 0.5f) + gy0;
                        for (int yy = 0; yy < gh; ++yy) {
                            const int ty = dy0 + yy;
                            if (ty < 0 || ty >= (int)kBubbleTexH) continue;
                            for (int xx = 0; xx < gw; ++xx) {
                                const int tx = dx0 + xx;
                                if (tx < 0 || tx >= (int)kBubbleTexW) continue;
                                const float covT = gb[(size_t)yy * gw + xx] / 255.0f;
                                if (covT <= 0.0f) continue;
                                BubbleBlend(texel((uint32_t)tx, (uint32_t)ty),
                                            1.0f, 1.0f, 1.0f, covT);
                            }
                        }
                    }
                    int adv = 0, lsb = 0;
                    stbtt_GetCodepointHMetrics(&g_bubbleFont, (unsigned char)*p, &adv, &lsb);
                    penX += adv * scale;
                    if (p[1]) penX += stbtt_GetCodepointKernAdvance(&g_bubbleFont,
                                        (unsigned char)*p, (unsigned char)p[1]) * scale;
                }
                lineTop += lineH;
            }
        }
    }

    // Debug: DXR_DUMP_BUBBLE → /tmp/avatar_bubble.png (once) to eyeball layout.
    static const bool s_dump = (getenv("DXR_DUMP_BUBBLE") != nullptr);
    static bool s_dumped = false;
    if (s_dump && !s_dumped) {
        s_dumped = true;
        stbi_write_png("/tmp/avatar_bubble.png", (int)subW, (int)subH, 4, buf.data(),
                       (int)(kBubbleTexW * 4));
    }
    return buf.data();
}

// Rasterize the bubble, acquire the window-space swapchain image, upload the
// staging buffer into it, and fill `outLayer` (the panel sub-rect mapped onto
// the full top-25% band). Returns true when the layer is ready to submit.
static bool BuildBubbleLayer(VkDevice dev, VkQueue queue, VkCommandPool pool,
                             uint32_t subW, uint32_t subH, int bandW, int bandH,
                             XrCompositionLayerLocal2DDXR& outLayer) {
    const uint8_t* px = RenderBubbleBitmap(subW, subH);
    if (!px) return false;

    XrSwapchainImageAcquireInfo bai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_bubbleSwapchain, &bai, &idx))) return false;
    XrSwapchainImageWaitInfo bwi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    bwi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g_bubbleSwapchain, &bwi))) return false;

    memcpy(g_bubbleStagingMapped, px, (size_t)kBubbleTexW * kBubbleTexH * 4);

    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cai, &cb) != VK_SUCCESS) {
        XrSwapchainImageReleaseInfo bri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_bubbleSwapchain, &bri);
        return false;
    }
    VkCommandBufferBeginInfo bgi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bgi);
    VkImage img = g_bubbleImages[idx].image;
    VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img; bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rg = {};
    rg.bufferRowLength = kBubbleTexW;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageOffset = {0, 0, 0};
    rg.imageExtent = {kBubbleTexW, kBubbleTexH, 1};
    vkCmdCopyBufferToImage(cb, g_bubbleStaging.buffer, img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cb);

    XrSwapchainImageReleaseInfo bri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_bubbleSwapchain, &bri);

    // Visible bubble = the panel sub-rect mapped onto the FULL top-25% band. The
    // Local2D layer's implicit M=0 mask over `rect` flattens that band to 2D so
    // the avatar stops weaving there; the panel fills it with the rounded pill.
    outLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR;
    outLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    outLayer.subImage.swapchain = g_bubbleSwapchain;
    outLayer.subImage.imageRect.offset = {0, 0};
    outLayer.subImage.imageRect.extent = {(int32_t)subW, (int32_t)subH};
    outLayer.subImage.imageArrayIndex = 0;
    outLayer.rect.offset = {0, 0};
    outLayer.rect.extent = {bandW, bandH};
    return true;
}

// ============================================================================
// Tiger-zone activation + per-frame render (XR_DXR_display_zones)
// ============================================================================

// One-shot activation: capabilities check + pre-sized zone swapchain. The zone
// swapchain is sized to the main atlas envelope (worst-case display×scale SBS);
// per-frame zone tiles are clamped into it (matches windows/main.cpp's
// fullscreen-worst-case pre-size). Requires the external window (has_external_
// window — same gate as Local2D/zone submission). Falls back to full-tile on any
// failure (g_hasDisplayZones cleared).
static bool TryActivateTigerZone(AppXrSession& xr) {
    if (g_zonesAttempted) return g_zonesActive;
    g_zonesAttempted = true;
    if (!g_hasDisplayZones || !xr.usingAppWindow) return false;
    if (g_pfnGetZoneCaps == nullptr || g_pfnGetZoneViewSize == nullptr) return false;

    XrDisplayZoneCapabilitiesDXR caps = {(XrStructureType)XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
    XrResult r = g_pfnGetZoneCaps(xr.session, &caps);
    if (XR_FAILED(r) || !caps.supported || caps.maxZones3D < 1) {
        LOG_WARN("[zones] caps rc=0x%x supported=%d maxZones3D=%u — tiger-zone disabled, "
                 "full-tile + Local2D fallback",
                 (unsigned)r, (int)caps.supported, caps.maxZones3D);
        g_hasDisplayZones = false;
        return false;
    }

    g_zoneSwW = xr.swapchain.width;   // atlas envelope: 2 SBS tiles wide
    g_zoneSwH = xr.swapchain.height;
    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = xr.swapchain.format;
    sci.sampleCount = 1;
    sci.width = g_zoneSwW;
    sci.height = g_zoneSwH;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &g_zoneSwapchain))) {
        LOG_WARN("[zones] zone swapchain create failed (%ux%u) — full-tile fallback", g_zoneSwW, g_zoneSwH);
        g_zoneSwapchain = XR_NULL_HANDLE;
        g_hasDisplayZones = false;
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_zoneSwapchain, 0, &n, nullptr);
    g_zoneImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g_zoneSwapchain, n, &n,
        (XrSwapchainImageBaseHeader*)g_zoneImages.data());

    g_zonesActive = true;
    LOG_INFO("[zones] tiger-zone ACTIVE: maxZones3D=%u, zone swapchain %ux%u (%u images)",
             caps.maxZones3D, g_zoneSwW, g_zoneSwH, n);
    return true;
}

// ============================================================================
// Per-frame view state shared by the render paths, the click-through pass and
// the XR_DXR_depth_budget content mask
// ============================================================================
static constexpr uint32_t kMaxViews = 8;

//! Whatever the frame's render path produced: the matrices it drew with, the
//! per-view shader far-cull dxr::ResolveClipPlanes resolved, and the client-px
//! rect the 3D content occupies. The silhouette pass re-renders the outermost
//! two from this, and the content mask is placed through `contentRect`.
struct FrameViews {
    uint32_t count = 0;
    float view[kMaxViews][16] = {};
    float proj[kMaxViews][16] = {};
    float projUnres[kMaxViews][16] = {};
    float clipFar[kMaxViews] = {};
    XrRect2Di contentRect = {{0, 0}, {0, 0}};
};

// XR_DXR_depth_budget v3 (#81 §6): the retained content-occupancy mask, reduced
// from the renderer's UNCLIPPED coverage pass once per rendered frame and
// re-chained verbatim on frames that skip rendering, so the runtime's ROI never
// goes stale. Render-thread-owned (this loop is one thread).
static std::vector<uint8_t> g_contentMaskCells;
static bool g_contentMaskChaining = false;   // one-time log on start/stop, never per frame
// Grid resolution for the chained mask: well under the extension's 512 cap and
// its 256 recommended ceiling. The runtime dilates by its own disparity band
// before measuring anything, so coarse is the point, not a shortcut.
static constexpr uint32_t kContentMaskGridCells = 64;

/*!
 * The `standalone` predicate dxr::ResolveClipPlanes takes: true when no
 * workspace controller is arbitrating behind us, which is when the hard
 * clip-at-the-ZDP rule applies. Derived from display_info v13's
 * `isRequestable` exactly as windows/main.cpp derives it — a non-controller
 * session under a workspace cannot request modes, and its per-app transparent
 * bridge is bypassed, so it must not self-clip.
 */
static bool AppIsStandalone(const AppXrSession& xr) {
    if (xr.renderingModeCount == 0) return true;
    const uint32_t i = (xr.activeRenderingMode < xr.renderingModeCount) ? xr.activeRenderingMode : 0u;
    return xr.renderingModeIsRequestable[i];
}

/*!
 * Fill one view's matrices from the runtime's render-ready pose/fov, resolving
 * near/far through the shared rear-depth-budget policy.
 *
 * `budget` is non-null only when the runtime actually filled the chained
 * struct this locate; nullptr reproduces the pre-extension rule bit-for-bit
 * (`farOffsetVH = transparent && standalone ? 0 : 1000`), which is what this
 * leg hard-coded as `farZ = ez` before.
 *
 * The coverage pass gets its OWN unrestricted-far projection (runtime#1470):
 * the rasterizer's NDC z > 1 clip is a second, budget-dependent clip the
 * fragment shader cannot see, so a mask derived from the real pass would be a
 * function of the budget that produced it and would oscillate against it.
 */
static void FillFrameView(FrameViews& fv, uint32_t i, const XrView& v,
                          const float rigPos[3], float vHeight,
                          const XrRearDepthBudgetDXR* budget, bool standalone) {
    mat4_view_from_xr_pose(fv.view[i], v.pose);
    // Identity rig orientation → RigLocalEyeZ reduces to the z difference.
    const float ez = fabsf(v.pose.position.z - rigPos[2]);
    const dxr::ClipPlanes clip =
        dxr::ResolveClipPlanes(ez, vHeight, budget, g_transparentBg, standalone);
    const dxr::ClipPlanes clipUnres =
        dxr::ResolveClipPlanes(ez, vHeight, /*budget=*/nullptr, /*transparent=*/false, standalone);
    mat4_from_xr_fov(fv.proj[i], v.fov, clip.near_z, clip.far_z);
    mat4_from_xr_fov(fv.projUnres[i], v.fov, clipUnres.near_z, clipUnres.far_z);
    // mat4_from_xr_fov emits GL [-1,1] clip depth; Vulkan clips [0,1].
    convert_projection_gl_to_zero_to_one(fv.proj[i]);
    convert_projection_gl_to_zero_to_one(fv.projUnres[i]);
    fv.clipFar[i] = clip.clipFar;
}

//! Publish the frame's CENTRE view/projection + content rect for the
//! double-click focus ray. The centre is the view centroid (the views are
//! spread about the rig centre) with view 0's orientation and fov — close
//! enough for a pick ray, and the same approximation the Windows leg makes.
static void PublishPickMatrices(const FrameViews& fv, const XrView* views, uint32_t n) {
    if (n == 0) { g_pickValid = false; return; }
    XrPosef centre = views[0].pose;
    if (n >= 2) {
        centre.position.x = (views[0].pose.position.x + views[n - 1].pose.position.x) * 0.5f;
        centre.position.y = (views[0].pose.position.y + views[n - 1].pose.position.y) * 0.5f;
        centre.position.z = (views[0].pose.position.z + views[n - 1].pose.position.z) * 0.5f;
    }
    mat4_view_from_xr_pose(g_pickView, centre);
    memcpy(g_pickProj, fv.proj[0], sizeof(g_pickProj));
    g_pickZoneX = fv.contentRect.offset.x;
    g_pickZoneY = fv.contentRect.offset.y;
    g_pickZoneW = fv.contentRect.extent.width;
    g_pickZoneH = fv.contentRect.extent.height;
    g_pickValid = (g_pickZoneW > 0 && g_pickZoneH > 0);
}

// Per-frame tiger-zone render: zone rect = bottom kAvatarCanvasFrac (75%) of the
// window; the avatar renders rig-framed INTO that rect (runtime Kooima via the
// zone-scoped locate) — no squish, 3D kept out of the top band. Fills projViews
// (zone-swapchain subimages) + outZone (zoneId/rect; caller clears .next before
// submit) + the view-0 silhouette matrices. Returns true on success; false →
// caller falls back to the full-tile path for this frame. Mirrors
// windows/main.cpp's zones branch + the cube_zones_vk_linux locate/render loop.
static bool RenderTigerZone(AppXrSession& xr, const XrFrameState& frameState,
                            std::vector<XrCompositionLayerProjectionView>& projViews,
                            XrDisplayZoneDXR& outZone, FrameViews& fv) {
    fv.count = 0;
    if (g_zoneSwapchain == XR_NULL_HANDLE || g_zoneSwW == 0 || g_zoneSwH == 0) return false;

    // Live client-window size (fixed portrait, but query so a resize tracks).
    uint32_t winW = 0, winH = 0;
    if (xr.xDisplay != nullptr && xr.xWindow != 0) {
        XWindowAttributes wa = {};
        if (XGetWindowAttributes(xr.xDisplay, xr.xWindow, &wa) && wa.width > 0 && wa.height > 0) {
            winW = (uint32_t)wa.width;
            winH = (uint32_t)wa.height;
        }
    }
    if (winW == 0 || winH == 0) { winW = xr.xWinW; winH = xr.xWinH; }
    if (winW == 0 || winH == 0) return false;

    // Zone rect = bottom 75% (client px, y-down); the top 25% is the bubble band.
    const int32_t topBand = (int32_t)((float)winH * (1.0f - kAvatarCanvasFrac) + 0.5f);
    XrDisplayRigDXR rig = {XR_TYPE_DISPLAY_RIG_DXR};
    rig.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    float rigPos[3]; ComputeRigPosition(rigPos);
    rig.pose.position = {rigPos[0], rigPos[1], rigPos[2]};
    rig.virtualDisplayHeight = CurrentVHeight();   // auto-fit ÷ wheel zoom
    // ipd and parallax move together — the one "3D effect strength" knob the
    // shared Windows input handler exposes on -/+ and Shift+wheel.
    rig.ipdFactor = g_ipdFactor; rig.parallaxFactor = g_ipdFactor; rig.perspectiveFactor = 1.0f;

    outZone = {(XrStructureType)XR_TYPE_DISPLAY_ZONE_DXR};
    outZone.next = &rig;                 // rig chained for the locate (framing)
    outZone.zoneId = 1;
    outZone.rect.offset = {0, topBand};
    outZone.rect.extent = {(int32_t)winW, (int32_t)winH - topBand};

    // Per-zone recommended view size, clamped to the pre-sized swapchain.
    const uint32_t eyeCount = 2;
    uint32_t tileW = 0, tileH = 0;
    XrExtent2Di rec = {};
    if (XR_SUCCEEDED(g_pfnGetZoneViewSize(xr.session, &outZone.rect, &rec)) &&
        rec.width > 0 && rec.height > 0) {
        tileW = (uint32_t)rec.width;
        tileH = (uint32_t)rec.height;
    } else {
        tileW = (uint32_t)outZone.rect.extent.width;
        tileH = (uint32_t)outZone.rect.extent.height;
    }
    if (tileW > g_zoneSwW / eyeCount) tileW = g_zoneSwW / eyeCount;
    if (tileH > g_zoneSwH) tileH = g_zoneSwH;
    if (tileW == 0) tileW = 1;
    if (tileH == 0) tileH = 1;

    // Zone-scoped locate: runtime Kooima framed to the rect → render-ready views.
    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.next = &outZone;
    locateInfo.viewConfigurationType = xr.viewConfigType;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = xr.localSpace;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    // XR_DXR_depth_budget: the runtime writes its advisory rear budget into
    // this chained struct. Zero-init means "untouched" — the `type` field is
    // the discriminator, so a runtime that ignores the chain is told apart
    // from one that deliberately returned an all-zero-vH clip.
    XrRearDepthBudgetDXR budget = {};
    if (xr.hasDepthBudget) dxr::ChainRearDepthBudget(viewState, budget);
    uint32_t viewCountOut = 0;
    XrView zoneViews[kMaxViews];
    for (uint32_t i = 0; i < kMaxViews; i++) zoneViews[i] = {XR_TYPE_VIEW};
    if (XR_FAILED(xrLocateViews(xr.session, &locateInfo, &viewState, kMaxViews, &viewCountOut, zoneViews)) ||
        viewCountOut == 0) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; LOG_WARN("[zones] zone-scoped xrLocateViews failed — full-tile fallback"); }
        return false;
    }
    // #1486/#1500 — submit min(active mode count, located, tiles). The zone
    // atlas is exactly `eyeCount` tiles wide (see the g_zoneSwW/eyeCount clamp
    // above), so the tile term is eyeCount; the wanted term is the ADOPTED
    // rendering mode's view count, so a 1-view mode submits one tile instead of
    // two. Never submit a view we did not locate.
    int zoneDisagreed = 0;
    const uint32_t zoneWanted = ActiveModeViewCount(xr);
    const uint32_t n = DxrClampSubmitViewCount(zoneWanted, viewCountOut, eyeCount, &zoneDisagreed);
    if (zoneDisagreed) {
        static bool s_warnedZoneClamp = false;
        if (!s_warnedZoneClamp) {
            s_warnedZoneClamp = true;
            LOG_WARN("[zones] view-count clamp: mode=%u located=%u tiles=%u -> submitting %u",
                     zoneWanted, viewCountOut, eyeCount, n);
        }
    }
    if (n == 0) return false;  // nothing submittable — fall back to the full-tile path

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_zoneSwapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g_zoneSwapchain, &wi))) {
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_zoneSwapchain, &ri);
        return false;
    }

    // XR_DXR_depth_budget: the runtime's advisory value, or nullptr when it
    // left the chained struct untouched (see ChainRearDepthBudget above).
    const XrRearDepthBudgetDXR* pBudget =
        (budget.type == (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR) ? &budget : nullptr;
    const bool standalone = AppIsStandalone(xr);
    const float vHeight = rig.virtualDisplayHeight;

    fv.count = n;
    fv.contentRect = outZone.rect;
    projViews.assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    for (uint32_t i = 0; i < n; i++) {
        FillFrameView(fv, i, zoneViews[i], rigPos, vHeight, pBudget, standalone);
        ModelRenderer::MaskProjections mp;
        mp.unrestricted = fv.projUnres[i];
        // Render the avatar into this eye's tile of the zone swapchain.
        g_modelRenderer.renderEye(
            g_zoneImages[imageIndex].image, (VkFormat)xr.swapchain.format,
            g_zoneSwW, g_zoneSwH,
            i * tileW, 0, tileW, tileH,
            fv.view[i], fv.proj[i], g_transparentBg,
            fv.clipFar[i], /*edgeFadePx=*/0.0f, &mp);

        projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projViews[i].subImage.swapchain = g_zoneSwapchain;
        projViews[i].subImage.imageRect.offset = {(int32_t)(i * tileW), 0};
        projViews[i].subImage.imageRect.extent = {(int32_t)tileW, (int32_t)tileH};
        projViews[i].subImage.imageArrayIndex = 0;
        projViews[i].pose = zoneViews[i].pose;
        projViews[i].fov = zoneViews[i].fov;
    }
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_zoneSwapchain, &ri);
    PublishPickMatrices(fv, zoneViews, n);
    return true;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    LOG_INFO("=== DisplayXR Avatar (Linux, app-owned ARGB window / transparent overlay) ===");

    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }
    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, physDevice, devExts, extStorage)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }
    // Negative-viewport Y-flip (matches the macOS/Windows raster convention).
    devExts.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    // Best-effort: create the app-owned transparent ARGB window before the
    // session so CreateSession can hand it over. Falls back to hosted-NULL
    // (headless CI, no compositor) — never fatal.
    CreateAppWindow(xr);

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    if (!CreateSpaces(xr) || !CreateSwapchain(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    std::vector<XrSwapchainImageVulkanKHR> swapchainImages(
        xr.swapchain.imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    { uint32_t count = xr.swapchain.imageCount;
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    // The cross-platform PBR renderer — the real portability surface exercised
    // by this build-green vehicle. Per-eye render target dimensions.
    if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                              queueFamilyIndex, xr.viewWidth, xr.viewHeight))
        LOG_WARN("model renderer init failed (no GPU on CI — expected)");

    // Load a CLI-supplied model, else the bundled avatar (copied next to the
    // exe by CMake — resolve it against the EXE dir, not the CWD: the run
    // scripts exec from arbitrary directories, which is exactly the avatar#21
    // "no model auto-loads" failure), else the built-in debug model. Mirrors
    // modelviewer's ExeDir/TryAutoLoadBundledScene and the macOS
    // _NSGetExecutablePath / Windows GetModuleFileNameA peers.
    std::string modelPath = (argc > 1) ? argv[1] : "";
    if (modelPath.empty()) {
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string bundled = std::string(dirname(buf)) + "/avatar.fbx";
            struct stat st;
            if (stat(bundled.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                modelPath = bundled;
            }
        }
        if (modelPath.empty()) modelPath = "avatar.fbx"; // CWD fallback (old behavior)
    }
    // Sweep every clip's bounds, not just the load-time one: the avatar switches
    // clips at runtime, so the fit has to know the union envelope (and the active
    // clip's own box — see ComputeAutoFit). Sticky on the renderer, but set at
    // each load site so no path can drift.
    g_modelRenderer.setBoundsSweepAllClips(true);
    if (g_modelRenderer.hasModel() || g_modelRenderer.loadModel(modelPath.c_str())) {
        LOG_INFO("Loaded model: %s", g_modelRenderer.modelPath().c_str());
    } else {
        LOG_WARN("No model at %s — using the built-in debug model", modelPath.c_str());
        g_modelRenderer.loadDebugModel();
    }
    g_modelRenderer.setPlainViewConvention(true);
    ComputeAutoFit();

    // Dynamic-recenter pins: default pin X+Y (avatar parity), leave Z as the
    // framed depth. DXR_RECENTER_PIN=XYZ|XY|Z|- overrides (the Linux control path).
    g_recenter.init(/*x=*/true, /*y=*/true, /*z=*/false);
    {
        char lbl[24];
        g_recenter.hudLabel(lbl, sizeof(lbl));
        LOG_INFO("Recenter: %s", lbl);
    }

    // Speech-bubble window-space swapchain (Local2D layer) — only when the
    // runtime advertises XR_DXR_local_3d_zone. A dedicated transient-reset command
    // pool feeds the per-frame staging→image upload. Guarded so the app still runs
    // (bubble simply absent) if the extension / swapchain / font is unavailable.
    // The Local2D speech bubble requires a session created WITH a window binding
    // (the runtime's verify_local_2d_layer gate: has_external_window on the VK
    // native path — oxr_session_frame_end.c). On the hosted-NULL fallback (no X
    // server / headless CI, or the extension absent) there is no external window,
    // so a submitted Local2D layer is rejected with XR_ERROR_LAYER_INVALID and
    // the WHOLE frame is dropped (the black-screen bug). Only build + submit the
    // bubble when we actually handed the runtime our window (xr.usingAppWindow);
    // otherwise the projection layer[0] submits alone and the avatar renders.
    VkCommandPool bubbleCmdPool = VK_NULL_HANDLE;
    if (g_hasLocal3DZone && xr.usingAppWindow) {
        g_bubbleReady = CreateBubbleSwapchain(xr, vkDevice, physDevice);
        if (g_bubbleReady) {
            VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &pci, nullptr, &bubbleCmdPool) != VK_SUCCESS) {
                LOG_WARN("Bubble command pool create failed — no speech bubble");
                bubbleCmdPool = VK_NULL_HANDLE;
                g_bubbleReady = false;
            }
        }
    } else if (g_hasLocal3DZone && !xr.usingAppWindow) {
        LOG_WARN("No external window (hosted-NULL) — Local2D speech bubble disabled "
                 "(a Local2D layer would be rejected by xrEndFrame and drop the frame)");
    } else {
        LOG_WARN("XR_DXR_local_3d_zone unavailable — no speech bubble");
    }

    LOG_INFO("=== Entering main loop (Ctrl+C to exit) ===");
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PollEvents(xr);
        PumpXEvents(xr);   // keyboard + mouse (see the control table there)
        PollFilePicker();  // async zenity result → loadModel + auto-fit

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Frame boundary for the renderer's per-view ring. MUST precede
        // updateAnimation: that rewrites the joint-matrix SSBO, which the
        // previous frame's views may still be reading — renderEye does not
        // drain the queue after each view. It is also what folds the
        // content-mask coverage readbacks in (see beginContentMaskFrame).
        // This leg never called it, so every kRingSlots-th renderEye fell back
        // to a full queue drain and the coverage pass could never publish.
        g_modelRenderer.beginFrame();
        g_modelRenderer.updateAnimation(dt);
        UpdateInteractive(dt, xr.displayHeightM);   // WASD/QE pan, focus ease

        if (!xr.sessionRunning) { usleep(100000); continue; }

        XrFrameState frameState = {XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(xr.session, &waitInfo, &frameState))) { xr.exitRequested = true; break; }
        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(xr.session, &beginInfo);

        bool rendered = false;
        bool zonesFrame = false;   // this frame used the tiger-zone path
        // Persists locate→submit: chained on the projection layer at xrEndFrame.
        XrDisplayZoneDXR tigerZone = {(XrStructureType)XR_TYPE_DISPLAY_ZONE_DXR};
        std::vector<XrCompositionLayerProjectionView> projViews;
        // What this frame drew with: consumed after the render by the
        // click-through silhouette pass and the depth-budget content mask.
        FrameViews fv;

        if (frameState.shouldRender) {
            // Lazily activate the tiger-zone once the session + window are up
            // (caps query + pre-sized zone swapchain). No-op after the first call.
            if (!g_zonesAttempted) TryActivateTigerZone(xr);

            // ── Tiger-zone path: avatar confined to the bottom-75% zone rect ──
            if (g_zonesActive) {
                zonesFrame = RenderTigerZone(xr, frameState, projViews, tigerZone, fv);
                rendered = zonesFrame;
            }

            // ── Full-tile fallback path (zones inactive / a transient zone-frame
            //    failure): the avatar renders across the whole tile + Local2D
            //    bubble. Preserved verbatim from the pre-tiger-zone build. ──
            if (!zonesFrame) {
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            // Chain the display view rig: the runtime returns render-ready
            // XrView{pose, fov} (camera AT the eye, off-axis Kooima FOV) so we
            // don't hand-roll the view math. This is the fix for the native-VK
            // path returning a display-plane (z≈0) view pose — with the rig the
            // pose sits at the eye (~0.5 m) like sim_display. Must be chained on
            // every locate (per-locate semantics). Identity pose = display plane
            // at the locate origin.
            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
            // Place the virtual display AT the model center, sized to the model
            // (windows/macOS approach) — the model renders at native scale and
            // the rig frames it. Identity orientation (forward = world -Z).
            displayRig.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            float dispRigPos[3]; ComputeRigPosition(dispRigPos);
            displayRig.pose.position = {dispRigPos[0], dispRigPos[1], dispRigPos[2]};
            displayRig.virtualDisplayHeight = CurrentVHeight();   // auto-fit ÷ wheel zoom
            displayRig.ipdFactor = g_ipdFactor;       // -/+ and Shift+wheel, in lockstep
            displayRig.parallaxFactor = g_ipdFactor;
            displayRig.perspectiveFactor = 1.0f;
            // Only chain the rig when display_info actually gave us panel dims —
            // the runtime needs them for the window-relative Kooima. Matches
            // cube_handle_vk_linux's useDisplayRig gate.
            const bool useDisplayRig = xr.hasViewRig && xr.displayWidthM > 0.0f;
            if (useDisplayRig) {
                locateInfo.next = &displayRig;
            }

            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            // XR_DXR_depth_budget — see the zone locate for the type-field
            // "untouched" discriminator.
            XrRearDepthBudgetDXR budget = {};
            if (xr.hasDepthBudget) dxr::ChainRearDepthBudget(viewState, budget);
            uint32_t viewCount = 0;
            xrLocateViews(xr.session, &locateInfo, &viewState, 0, &viewCount, nullptr);
            if (viewCount == 0) viewCount = 2;
            std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
            XrResult loc = xrLocateViews(xr.session, &locateInfo, &viewState, viewCount, &viewCount, views.data());

            // Render whenever the locate succeeds — the view pose/FOV come from the
            // display rig above (render-ready), matching modelviewer/gaussiansplat.
            if (XR_SUCCEEDED(loc)) {
                XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                uint32_t imageIndex = 0;
                if (XR_SUCCEEDED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &acqInfo, &imageIndex))) {
                    XrSwapchainImageWaitInfo swWait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    swWait.timeout = XR_INFINITE_DURATION;
                    if (XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &swWait))) {
                        rendered = true;
                        // #1486/#1500 — submit min(active mode count, located,
                        // tiles). The wanted count is the ADOPTED rendering
                        // mode's (see ActiveModeViewCount), not a hardcoded 2:
                        // in a 1-view mode this leg used to submit two views
                        // into a one-tile atlas. The atlas is a fixed 2-tile
                        // horizontal split (see the swapchain.width/eyeCount
                        // clamp below), so a 4-view mode clamps back to 2 and
                        // logs once — this leg cannot render Quad until the tile
                        // layout follows the mode's cols x rows grid.
                        uint32_t eyeCount;
                        {
                            const uint32_t wanted = ActiveModeViewCount(xr);
                            int disagreed = 0;
                            eyeCount = DxrClampSubmitViewCount(wanted, viewCount, 2u, &disagreed);
                            if (disagreed) {
                                static bool s_warnedViewClamp = false;
                                if (!s_warnedViewClamp) {
                                    s_warnedViewClamp = true;
                                    LOG_WARN("View-count clamp: mode=%u located=%u tiles=2 -> submitting %u",
                                             wanted, viewCount, eyeCount);
                                }
                            }
                        }
                        // Unreachable on this path (the two-call above guarantees
                        // viewCount >= 1 whenever the locate succeeded), but a
                        // projection layer with viewCount 0 is itself invalid, so
                        // never let the render loop degenerate.
                        if (eyeCount == 0) eyeCount = 1;
                        // Per-eye RENDER TILE = window × recommendedViewScale
                        // (docs/specs/runtime/multiview-tiling.md + ADR-010/030):
                        // the swapchain is the worst-case display×scale envelope,
                        // but a WINDOWED extension app renders view = window×scale
                        // and the compositor crops. Using the frozen recommended
                        // (display-sized) tile made the window-relative Kooima fov
                        // render oversized + off-center when windowed. Query the
                        // LIVE window each frame; clamp to the swapchain envelope.
                        // Falls back to the recommended per-view dims on the
                        // hosted-NULL path (no app window) or if display_info is
                        // absent (viewScale stays 1.0). Mirrors cube_handle_vk_linux.
                        uint32_t winW = 0, winH = 0;
                        if (xr.usingAppWindow && xr.xDisplay != nullptr && xr.xWindow != 0) {
                            XWindowAttributes wa = {};
                            if (XGetWindowAttributes(xr.xDisplay, xr.xWindow, &wa) &&
                                wa.width > 0 && wa.height > 0) {
                                winW = (uint32_t)wa.width;
                                winH = (uint32_t)wa.height;
                            }
                        }
                        uint32_t eyeW, eyeH;
                        if (winW > 0 && winH > 0) {
                            eyeW = (uint32_t)(winW * xr.viewScaleX);
                            eyeH = (uint32_t)(winH * xr.viewScaleY);
                        } else {
                            eyeW = xr.viewWidth;   // fallback: recommended (fullscreen envelope)
                            eyeH = xr.viewHeight;
                        }
                        if (eyeW == 0) eyeW = 1;
                        if (eyeH == 0) eyeH = 1;
                        // Clamp to the worst-case swapchain (SBS packs 2 tiles wide).
                        if (eyeW > xr.swapchain.width / eyeCount) eyeW = xr.swapchain.width / eyeCount;
                        if (eyeH > xr.swapchain.height) eyeH = xr.swapchain.height;
                        projViews.resize(eyeCount, {});
                        // Foreground clip: far = eye→display distance, so the far
                        // plane coincides with the virtual display plane, with the
                        // runtime's advisory rear budget allowed to push it back.
                        // dxr::ResolveClipPlanes owns that whole rule — nullptr
                        // budget reproduces the old hard `farZ = ez` exactly.
                        const XrRearDepthBudgetDXR* pBudget =
                            (budget.type == (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR) ? &budget : nullptr;
                        const bool standalone = AppIsStandalone(xr);
                        const float vHeight = displayRig.virtualDisplayHeight;
                        float rigPos[3];
                        ComputeRigPosition(rigPos);
                        fv.count = (eyeCount < kMaxViews) ? eyeCount : kMaxViews;
                        // No zone on this path: the avatar occupies the whole tile,
                        // so the content rect is the whole window.
                        fv.contentRect.offset = {0, 0};
                        fv.contentRect.extent = {(int32_t)(winW > 0 ? winW : eyeW),
                                                 (int32_t)(winH > 0 ? winH : eyeH)};
                        for (uint32_t i = 0; i < eyeCount; i++) {
                            // Render at native model scale — the rig (placed at the
                            // model center, sized to the model) owns the framing. No
                            // model-fit baked into the view.
                            const uint32_t fi = (i < kMaxViews) ? i : kMaxViews - 1;
                            FillFrameView(fv, fi, views[i], rigPos, vHeight, pBudget, standalone);
                            ModelRenderer::MaskProjections mp;
                            mp.unrestricted = fv.projUnres[fi];
                            // Draw the avatar into this eye's SBS viewport region.
                            g_modelRenderer.renderEye(
                                swapchainImages[imageIndex].image,
                                (VkFormat)xr.swapchain.format,
                                xr.swapchain.width, xr.swapchain.height,
                                i * eyeW, 0, eyeW, eyeH,
                                fv.view[fi], fv.proj[fi],
                                g_transparentBg,
                                fv.clipFar[fi], /*edgeFadePx=*/0.0f, &mp);

                            projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projViews[i].subImage.swapchain = xr.swapchain.swapchain;
                            projViews[i].subImage.imageRect.offset = {(int32_t)(i * eyeW), 0};
                            projViews[i].subImage.imageRect.extent = {(int32_t)eyeW, (int32_t)eyeH};
                            projViews[i].subImage.imageArrayIndex = 0;
                            projViews[i].pose = views[i].pose;
                            projViews[i].fov = views[i].fov;
                        }
                        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        xrReleaseSwapchainImage(xr.swapchain.swapchain, &relInfo);
                        PublishPickMatrices(fv, views.data(), eyeCount);
                    }
                }
            }
            } // end if (!zonesFrame) — full-tile fallback path
        }

        // ── Live client geometry, queried ONCE per frame ────────────────────
        // On the app-owned path the runtime repositions/resizes the overlay
        // onto the target panel without a ConfigureNotify, so xr.xWinW/xWinH
        // hold the stale creation size. Everything below (the click-through
        // region, the content mask, the bubble band) needs the real rect.
        unsigned int winPxW = xr.xWinW, winPxH = xr.xWinH;
        if (xr.usingAppWindow && xr.xDisplay != nullptr && xr.xWindow != 0) {
            Window gRoot; int gx, gy; unsigned int gw, gh, gbw, gd;
            if (XGetGeometry(xr.xDisplay, xr.xWindow, &gRoot, &gx, &gy, &gw, &gh, &gbw, &gd) &&
                gw > 0 && gh > 0) {
                winPxW = gw; winPxH = gh;
            }
        }
        if (winPxW == 0 || winPxH == 0) { winPxW = xr.viewWidth; winPxH = xr.viewHeight; }
        const int bubbleBandW = (int)winPxW;
        const int bubbleBandH = (winPxH > 0) ? (int)((float)winPxH * (1.0f - kAvatarCanvasFrac)) : 1;
        const bool bubbleWillSubmit = rendered && g_bubbleReady && g_hasLocal3DZone &&
                                      xr.usingAppWindow && bubbleCmdPool != VK_NULL_HANDLE;

        // ── Click-through region + XR_DXR_depth_budget content mask ─────────
        // Runs EVERY frame now. It used to be throttled to ~4 Hz because the
        // readback was a synchronous vkQueueWaitIdle; it is pipelined behind a
        // fence and HOST_CACHED, and the region is not only a hit mask — the
        // XShape input region decides where the window exists at all, so a
        // stale one clips the leading edge of a moving avatar.
        // DXR_AVATAR_SIL_EVERY_N=<n> restores a throttle (n=1, every frame, is
        // the default and the Windows behaviour). Kept as an escape hatch
        // because the every-frame cost has been measured on Windows hardware
        // but not yet on Linux; at n>1 the region lags a moving avatar.
        static const uint32_t s_silEveryN = []() {
            const char* e = getenv("DXR_AVATAR_SIL_EVERY_N");
            if (e == nullptr || e[0] == '\0') return 1u;
            const long v = strtol(e, nullptr, 10);
            return (v >= 1 && v <= 60) ? (uint32_t)v : 1u;
        }();
        static uint32_t s_silFrame = 0;
        const bool silThisFrame = (s_silEveryN == 1) || ((s_silFrame++ % s_silEveryN) == 0);

        if (rendered && xr.usingAppWindow && fv.count > 0 && silThisFrame) {
            // runtime#1470: arm the renderer's coverage-only pass for exactly
            // the two views the silhouette pass is about to render. It redraws
            // the avatar with the far clip absent, so the content mask
            // describes the silhouette AS IT WOULD RENDER AT AN UNRESTRICTED
            // BUDGET — the rendered alpha cannot, because it is a function of
            // the budget the runtime published and the two oscillate against
            // each other. beginFrame() disarms. Gated on the RUNTIME's
            // reported version, never this app's vendored SPEC_VERSION.
            // Opaque or decorated frames are excluded: ResolveClipPlanes leaves
            // an opaque frame unrestricted (so the ROI is moot) and
            // ClickthroughUpdate returns without rendering in both cases, which
            // would leave the coverage stale.
            const bool wantContentMask = xr.hasDepthBudget && xr.depthBudgetVersion >= 3 &&
                                         zonesFrame && g_transparentBg && !g_decorated;
            g_modelRenderer.beginContentMaskFrame(wantContentMask);

            ClickthroughParams cp;
            cp.dev = vkDevice;
            cp.phys = physDevice;
            cp.queue = graphicsQueue;
            cp.queueFamily = queueFamilyIndex;
            cp.renderer = &g_modelRenderer;
            cp.dpy = xr.xDisplay;
            cp.win = xr.xWindow;
            cp.winW = winPxW;
            cp.winH = winPxH;
            cp.viewMats = fv.view;
            cp.projMats = fv.proj;
            cp.projMatsUnres = fv.projUnres;
            cp.clipFars = fv.clipFar;
            cp.numViews = fv.count;
            // Where the avatar actually is: the zone rect on the zones path,
            // the whole window on the full-tile fallback.
            cp.avatarFrac = (winPxH > 0 && fv.contentRect.extent.height > 0)
                                ? (float)fv.contentRect.extent.height / (float)winPxH
                                : 1.0f;
            if (cp.avatarFrac <= 0.0f || cp.avatarFrac > 1.0f) cp.avatarFrac = 1.0f;
            cp.transparentBg = g_transparentBg;
            cp.decorated = g_decorated;
            cp.bubbleVisible = bubbleWillSubmit;
            cp.bubbleX = 0;
            cp.bubbleY = 0;
            cp.bubbleW = bubbleBandW;
            cp.bubbleH = bubbleBandH;
            ClickthroughUpdate(cp);

            // XR_DXR_depth_budget v3 (#81 §6): reduce the frame's UNCLIPPED
            // coverage to the extension's occupancy grid. Deliberately NOT the
            // click-through coverage — that one legitimately wants the
            // post-clip alpha (the window must not be drawn or clickable where
            // nothing rendered); the budget mask must not, or it feeds the
            // runtime's own hysteresis (runtime#1470). Null until the first
            // armed frame has been read back — chain nothing rather than fall
            // back to the clipped alpha, so the runtime uses its coarser but
            // clip-independent fallback.
            if (wantContentMask) {
                std::vector<uint8_t> maskCells;
                bool haveMask = false;
                const uint8_t* cov = g_modelRenderer.contentMaskCoverage();
                if (cov != nullptr) {
                    haveMask = dxr::ContentMaskFromCoverage(
                        cov, ModelRenderer::kContentMaskCovW, ModelRenderer::kContentMaskCovH,
                        ModelRenderer::kContentMaskCovW, (uint32_t)winPxW, (uint32_t)winPxH,
                        &fv.contentRect, kContentMaskGridCells, kContentMaskGridCells, maskCells);
                }
                const uint32_t occupied = haveMask ? dxr::ContentMaskCoverageCells(maskCells) : 0;
                if (occupied > 0) {
                    g_contentMaskCells.swap(maskCells);
                    if (!g_contentMaskChaining) {
                        g_contentMaskChaining = true;
                        LOG_INFO("XR_DXR_depth_budget v3: content mask chaining started "
                                 "(%ux%u cells, %u occupied)",
                                 kContentMaskGridCells, kContentMaskGridCells, occupied);
                    }
                } else {
                    g_contentMaskCells.clear();
                }
            }
            if (!wantContentMask || g_contentMaskCells.empty()) {
                if (g_contentMaskChaining) {
                    g_contentMaskChaining = false;
                    g_contentMaskCells.clear();
                    LOG_INFO("XR_DXR_depth_budget v3: content mask chaining stopped "
                             "— falling back to the runtime's own ROI");
                }
            }
        }

        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        // Keep the existing blend mode as-is (the runtime keys the transparent
        // overlay off the xlib binding's transparentBackgroundEnabled flag, not
        // this mode) — out of scope for the bubble port.
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerLocal2DDXR bubbleLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
        const XrCompositionLayerBaseHeader* layers[2];
        uint32_t layerN = 0;
        if (rendered) {
            projLayer.space = xr.localSpace;
            projLayer.viewCount = (uint32_t)projViews.size();
            projLayer.views = projViews.data();
            if (zonesFrame) {
                // Same XrDisplayZoneDXR instance as the zone locate, rig chain
                // CLEARED (windows/main.cpp semantics) — this makes it a ZONES
                // frame: the runtime weaves the avatar into the bottom-75% rect and
                // auto-derives the wish (feathered bottom-75%), leaving the top 25%
                // flat for the Local2D bubble. No mask object.
                tigerZone.next = nullptr;
                projLayer.next = &tigerZone;
            }
            layers[layerN++] = (XrCompositionLayerBaseHeader*)&projLayer;

            // ── Speech bubble: a flat 2D nameplate pill in the top ~25% band,
            //    submitted as a single Local2D layer. The panel is drawn into the
            //    LARGEST band-aspect sub-rect of the fixed bubble texture, then
            //    mapped onto the full band — equal scale on both axes so corners
            //    stay round and text unstretched on any resize (same fit math as
            //    windows/main.cpp:1887-1900). ──
            // Gate the Local2D bubble on the external window (has_external_window)
            // so xrEndFrame accepts the frame — see the swapchain-create comment.
            if (g_bubbleReady && g_hasLocal3DZone && xr.usingAppWindow &&
                bubbleCmdPool != VK_NULL_HANDLE) {
                // Band rect from the once-per-frame geometry query above — the
                // click-through region unions the SAME rect, so the two cannot
                // drift apart.
                const int bandH = bubbleBandH;
                const int bandW = bubbleBandW;
                const float bandAR = (float)bandW / (float)(bandH > 0 ? bandH : 1);
                const float texAR = (float)kBubbleTexW / (float)kBubbleTexH;
                uint32_t subW, subH;
                if (bandAR >= texAR) { subW = kBubbleTexW; subH = (uint32_t)((float)kBubbleTexW / bandAR + 0.5f); }
                else                 { subH = kBubbleTexH; subW = (uint32_t)((float)kBubbleTexH * bandAR + 0.5f); }
                if (subW < 2) subW = 2; else if (subW > kBubbleTexW) subW = kBubbleTexW;
                if (subH < 2) subH = 2; else if (subH > kBubbleTexH) subH = kBubbleTexH;

                if (BuildBubbleLayer(vkDevice, graphicsQueue, bubbleCmdPool,
                                     subW, subH, bandW, bandH, bubbleLayer)) {
                    layers[layerN++] = (XrCompositionLayerBaseHeader*)&bubbleLayer;
                }
            }

            endInfo.layerCount = layerN;
            endInfo.layers = layers;
        }
        // DXR_ZONES_VALIDATE=1 chains the strict locate/submit pairing validate
        // bit (bring-up diagnostics). Default: nothing chained = AUTO wish
        // (feathered bottom-75%), matching windows/main.cpp + cube_zones_vk_linux.
        XrDisplayZonesFrameEndInfoDXR zonesEnd = {(XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR};
        if (rendered && zonesFrame && AvatarZonesValidate()) {
            zonesEnd.flags = XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR;
            zonesEnd.wishMask = XR_NULL_HANDLE;
            endInfo.next = &zonesEnd;
        }
        // XR_DXR_depth_budget v3: chain the retained content mask AFTER the
        // zones struct (ChainContentMask prepends, keeping whatever was linked
        // reachable). Chained on EVERY frame it is non-empty, including frames
        // that skipped rendering — the runtime's ROI must not go stale just
        // because we had nothing new to draw. The vector is file-scope, so it
        // outlives the xrEndFrame call as the helper requires.
        XrContentMaskDXR contentMaskDXR = {};
        if (xr.hasDepthBudget && xr.depthBudgetVersion >= 3 && !g_contentMaskCells.empty()) {
            dxr::ChainContentMask(endInfo, contentMaskDXR, g_contentMaskCells,
                                  kContentMaskGridCells, kContentMaskGridCells);
        }
        xrEndFrame(xr.session, &endInfo);
    }

    LOG_INFO("=== Shutting down ===");
    // Teardown order matters (avatar#21 SIGABRT): release the global
    // renderer's device objects and idle the device BEFORE destroying it —
    // otherwise ~ModelRenderer's static-dtor cleanup() runs vkDeviceWaitIdle
    // + resource destroys on a dead VkDevice. Mirrors modelviewer.
    g_modelRenderer.cleanup();
    if (vkDevice) vkDeviceWaitIdle(vkDevice);
    if (vkDevice) ClickthroughDestroy(vkDevice); // silhouette scratch + pool
    // Speech-bubble teardown (before the swapchains/session go in CleanupOpenXR).
    if (vkDevice && bubbleCmdPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vkDevice, bubbleCmdPool, nullptr);
    if (g_bubbleSwapchain != XR_NULL_HANDLE) { xrDestroySwapchain(g_bubbleSwapchain); g_bubbleSwapchain = XR_NULL_HANDLE; }
    if (g_zoneSwapchain != XR_NULL_HANDLE) { xrDestroySwapchain(g_zoneSwapchain); g_zoneSwapchain = XR_NULL_HANDLE; }
    if (vkDevice && g_bubbleStaging.buffer != VK_NULL_HANDLE) {
        if (g_bubbleStagingMapped) { vkUnmapMemory(vkDevice, g_bubbleStaging.memory); g_bubbleStagingMapped = nullptr; }
        modelDestroyBuffer(vkDevice, g_bubbleStaging);
    }
    CleanupOpenXR(xr);
    if (vkDevice) vkDestroyDevice(vkDevice, nullptr);
    if (vkInstance) vkDestroyInstance(vkInstance, nullptr);
    LOG_INFO("Clean exit");
    return 0;
}
