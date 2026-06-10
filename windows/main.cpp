// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR 3DGS OpenXR Ext VK - glTF 2.0 PBR model viewer with OpenXR (Vulkan)
 *
 * Renders glTF 2.0 models on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk with the cube/grid renderer replaced by
 * a 3DGS.cpp compute pipeline.  Features a "Load Scene" button as a
 * window-space layer overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM for WM_NCHITTEST
#include <commdlg.h>
#include <shlwapi.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "model_renderer.h"
#include "model_vulkan_utils.h"   // scratch image/buffer for the click-through silhouette
#include "display3d_view.h"
#include "projection_depth.h"

#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

// stb_image_write implementation is linked from displayxr-common; used by the
// debug silhouette dump in UpdateSilhouette.
extern "C" int stbi_write_png(char const* filename, int w, int h, int comp,
                              const void* data, int stride_in_bytes);

static const char* APP_NAME = "avatar_handle_vk_win";

static const wchar_t* WINDOW_CLASS = L"DisplayXRAvatarClass";
static const wchar_t* WINDOW_TITLE = L"DisplayXR 3D Avatar";

// HUD overlay fractions. Layer spans full window height so chrome buttons
// can sit at the window top while the info panel anchors to the bottom-left
// (matching the macOS demo's split). The vk_native compositor now uses an
// alpha-blended draw pass for window-space layers, so the empty middle of
// the texture stays invisible. Font sizing is anchored to the legacy
// 0.5-fraction so text doesn't grow with the taller texture.
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 1.0f;
static const float HUD_FONT_BASE_FRACTION = 0.50f;

// ── Top button bar ────────────────────────────────────────────────────────
// All chrome buttons live in ONE full-width window-space layer at the top:
// Open + Mode packed at the left, the Animation pill pinned to the right, and a
// transparent center so the model shows through. This replaces the old split
// (Open/Mode baked into the HUD layer + Animation on its own separate layer) —
// per runtime issue #389: group co-planar controls into a single layer and keep
// the HUD info panel as its own (toggleable) layer. Positions below are absolute
// window-fractions, used both for hit-testing and for placing the pills inside
// the bar texture (the bar layer spans the full window width, so window-x maps
// straight onto bar-texture-x).
static const float OPEN_BTN_X_FRACTION = 0.010f;
static const float OPEN_BTN_WIDTH_FRACTION  = 0.060f;

static const float MODE_BTN_X_FRACTION = 0.075f;
static const float MODE_BTN_WIDTH_FRACTION  = 0.140f;

// Animation pill — right-aligned within the bar. Only drawn/clickable when the
// model has clips. Label = current clip name, or "Paused"; click = next clip
// (same as 'N').
static const float ANIM_BTN_WIDTH_FRACTION  = 0.140f;
static const float ANIM_BTN_MARGIN_FRACTION = 0.010f;
static inline float AnimBtnXFraction() {
    return 1.0f - ANIM_BTN_WIDTH_FRACTION - ANIM_BTN_MARGIN_FRACTION;
}

// Bar swapchain texture (wide + thin) and its window-space layer geometry. The
// layer spans the full window width; its height preserves the texture aspect so
// the pills aren't distorted as the tile is resized. The pills fill ~70% of the
// bar height, vertically centered.
static const uint32_t BTN_BAR_TEX_W = 1920;
static const uint32_t BTN_BAR_TEX_H = 56;
static const uint32_t BTN_BAR_FONT_BASE = BTN_BAR_TEX_H * 14;
static const float    BTN_BAR_Y_FRACTION = 0.008f;
static inline float BtnBarHeightFraction(uint32_t windowW, uint32_t windowH) {
    if (windowW == 0 || windowH == 0) return 0.05f;
    const float windowAR = (float)windowW / (float)windowH;
    const float texAR = (float)BTN_BAR_TEX_W / (float)BTN_BAR_TEX_H;
    return windowAR / texAR;  // layer width fraction = 1.0
}


// sim_display output mode switching (legacy — replaced by unified rendering mode)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

// Global state
static InputState g_inputState;
// Standalone demo: bare TAB toggles the HUD (displayxr::common defaults to
// SHIFT+TAB so runtime test apps don't shadow the workspace shell's
// focus-cycle binding).
static const bool g_inputInit = [] {
    g_inputState.hudToggleRequiresShift = false;
    return true;
}();
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// 3DGS state
static ModelRenderer g_modelRenderer;
// Cross-thread scene-load queue: the file dialog runs on the main (message-pump)
// thread, but the actual ModelRenderer::loadScene() submits Vulkan work on the
// graphics queue and so MUST run on the same thread that drives per-frame
// rendering — otherwise concurrent vkQueueSubmit/vkQueueWaitIdle from two
// threads on a single VkQueue is undefined behaviour and crashes some drivers
// (NVIDIA in particular). Main thread posts the picked path here; the render
// thread picks it up between frames.
static std::atomic<bool> g_loadRequested{false};
static std::string g_pendingLoadPath;
static std::mutex g_pendingLoadPathMutex;
// 'I' key: capture the multi-view atlas region (cols × rows × renderW × renderH)
// of the swapchain to a PNG in %USERPROFILE%\Pictures\DisplayXR\. Skipped for
// 1×1 (mono) layouts. Helper lives in test_apps/common/atlas_capture*.
static std::atomic<bool> g_captureAtlasRequested{false};
// Transparent background. The avatar app is transparent by DEFAULT — the whole
// point is a character floating over the desktop. Always-on session-level
// transparency is wired at xrCreateSession; this flag flips the renderer's
// output alpha (1 → 1-T) so background-uncovered pixels punch through to the
// desktop, and (when set) enables per-eye foreground clipping at the display
// plane. Ctrl+T still toggles it for debugging the opaque path.
static std::atomic<bool> g_transparentBg{true};
static std::string g_loadedFileName;
static std::mutex g_sceneMutex;
// True when the loaded model has animation clips — gates the animation button
// layer + its click hit-test (read on the UI thread, set on load).
static std::atomic<bool> g_hasAnimations{false};

// Animation-button window-space layer resources: created in main() (when the
// HUD swapchain — i.e. window-space layers — is available), used by the render
// thread. The swapchain is app-owned state (displayxr::common's
// XrSessionManager carries no app-named fields, #396 W4) — created via the
// lib's CreateWindowSpaceSwapchain generic, destroyed before CleanupOpenXR.
static SwapchainInfo  g_animBtnSwapchain;                // app-owned window-space swapchain
static bool           g_hasAnimBtnSwapchain = false;
static HudRenderer    g_animBtnHud = {};                 // own D3D11 text renderer (256×80)
static bool           g_animBtnReady = false;            // all resources created
static VkBuffer       g_animBtnStaging = VK_NULL_HANDLE;
static VkDeviceMemory g_animBtnStagingMem = VK_NULL_HANDLE;
static void*          g_animBtnStagingMapped = nullptr;
static VkCommandPool  g_animBtnCmdPool = VK_NULL_HANDLE;
static std::vector<XrSwapchainImageVulkanKHR> g_animBtnSwapImages;

// Fallback vHeight when no scene is loaded or auto-fit hits a degenerate
// extent. Matches macOS demo's kDefaultVirtualDisplayHeightM (1.5m).
static constexpr float kFallbackVirtualDisplayHeightM = 1.5f;
// Initial virtual-display height as a multiple of the model's height: the
// display-centric rig frames the (centered) model with 1.4× its height, i.e.
// ~20% headroom top and bottom — enough that the window title bar doesn't
// clip the subject.
static constexpr float kAutoFitVerticalComfort = 1.4f;

// Cached auto-fit pose for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kFallbackVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static std::atomic<bool> g_fitValid{false};

// Compute robust scene bounds (5th–95th percentile per axis) and stage
// new display-rig pose + vHeight on g_inputState. Display orientation is
// kept identity (forward = world −Z): splats have no canonical front, and
// any heuristic (PCA, etc.) can pick the wrong side; the user can rotate
// with mouse drag from a predictable starting pose.
// Caller must hold g_sceneMutex (we read pickData_ from the renderer).
static void ApplyAutoFitForLoadedScene_locked() {
    // Gate the right-justified animation button on whether this model has clips.
    g_hasAnimations.store(g_modelRenderer.hasAnimations());
    float center[3], extent[3];
    // Full model AABB: center for the rig position, extent[1] for the height fit.
    bool ok = g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent);
    if (ok) {
        g_fitCenter[0] = center[0];
        g_fitCenter[1] = center[1];
        g_fitCenter[2] = center[2];
        float vh = extent[1] * kAutoFitVerticalComfort;
        // Degenerate scene (all splats in a thin slice) — fall back to a
        // sensible vHeight rather than failing the fit. Mirrors macOS:1399.
        if (!(vh > 1e-3f)) vh = kFallbackVirtualDisplayHeightM;
        g_fitVHeight = vh;
        // Anchor at yaw=0 and trust the loader's RUB convention (PLY loader
        // converts RDF+X-mirror → RUB at load time; SPZ is RUB-native and
        // SuperSplat-authored scenes already face −Z at yaw=0). Matches
        // macOS:1407 — the user can drag with LMB if a particular asset's
        // authored orientation is off.
        g_fitYaw = 0.0f;
        LOG_INFO("Auto-fit: center=(%.3f, %.3f, %.3f) extent=(%.3f, %.3f, %.3f) vHeight=%.3f yaw=%.0fdeg",
                 center[0], center[1], center[2],
                 extent[0], extent[1], extent[2], vh, g_fitYaw * 57.2957795f);
    }
    g_fitValid.store(ok);

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputState.cameraPosX = ok ? g_fitCenter[0] : 0.0f;
    g_inputState.cameraPosY = ok ? g_fitCenter[1] : 0.0f;
    g_inputState.cameraPosZ = ok ? g_fitCenter[2] : 0.0f;
    g_inputState.yaw = ok ? g_fitYaw : 0.0f;
    g_inputState.pitch = 0.0f;
    g_inputState.viewParams.virtualDisplayHeight = ok ? g_fitVHeight : kFallbackVirtualDisplayHeightM;
    g_inputState.viewParams.scaleFactor = 1.0f;

    // Per-format orientation correction is now done at load time (PLY loader
    // converts RDF+X-mirror → canonical RUB; SPZ loader uses RUB natively).
    // Renderer's ModelRenderer::updateUniforms negates the Y row of proj_mat to
    // match the +Y-up convention. No runtime view-stage flips needed.

    // Route the first post-load frame through the same reset path Space uses,
    // so app-start view params (perspectiveFactor, scaleFactor, etc.) match
    // the Space-reset state.
    g_inputState.resetViewRequested = true;

    // Treat scene load as a fresh user interaction so the auto-orbit idle
    // timer restarts. Without this, an asset loaded after the 10s idle
    // threshold starts rotating immediately on first display.
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
        g_inputState.animationActive = false;
    }
}

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        g_savedWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

// Borderless ⇄ decorated toggle (B key). The avatar floats chrome-free
// (WS_POPUP) by default so nothing but the character is visible over the
// desktop. Toggling decoration on (WS_OVERLAPPEDWINDOW) restores the title bar
// + sizing border so the user can drag/resize the window with the OS, then
// toggle back to borderless — this replaces the Unity build's in-app
// Ctrl+Shift+L virtual-window editor. The client rect is preserved across the
// toggle (AdjustWindowRect compensates for the added/removed non-client frame)
// so the avatar doesn't jump or rescale.
static bool g_decorated = false;  // false = borderless (default)

static void ToggleDecoration(HWND hwnd) {
    if (g_fullscreen) return;  // decoration is meaningless in fullscreen

    // Current client rect in screen space — what we want to keep fixed.
    RECT client = {};
    GetClientRect(hwnd, &client);
    POINT topLeft = { client.left, client.top };
    ClientToScreen(hwnd, &topLeft);
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;

    g_decorated = !g_decorated;
    const DWORD style = (g_decorated ? WS_OVERLAPPEDWINDOW : WS_POPUP) | WS_VISIBLE;
    SetWindowLong(hwnd, GWL_STYLE, style);

    // Expand the desired client rect by the new frame so the client area stays
    // put at the same screen position and size.
    RECT want = { topLeft.x, topLeft.y, topLeft.x + clientW, topLeft.y + clientH };
    AdjustWindowRect(&want, style, FALSE);
    // HWND_TOPMOST: keep the avatar above other windows across the decoration
    // toggle (it floats on top whether borderless or framed).
    SetWindowPos(hwnd, HWND_TOPMOST, want.left, want.top,
        want.right - want.left, want.bottom - want.top,
        SWP_FRAMECHANGED);
    LOG_INFO("Window decoration: %s (B)", g_decorated ? "ON (move/resize)" : "OFF (borderless)");
}

static bool PointInFractionRect(int mouseX, int mouseY, int windowW, int windowH,
                                float xf, float yf, float wf, float hf) {
    if (windowW <= 0 || windowH <= 0) return false;
    float fx = (float)mouseX / (float)windowW;
    float fy = (float)mouseY / (float)windowH;
    return (fx >= xf && fx <= xf + wf && fy >= yf && fy <= yf + hf);
}

// All three buttons share the top bar's vertical band [BTN_BAR_Y, +barHeight];
// each owns its own x-column. Keeps hit-testing aligned with the rendered pills.
static bool IsClickOnLoadButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        OPEN_BTN_X_FRACTION, BTN_BAR_Y_FRACTION,
        OPEN_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

static bool IsClickOnModeButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        MODE_BTN_X_FRACTION, BTN_BAR_Y_FRACTION,
        MODE_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

static bool IsClickOnAnimButton(int mouseX, int mouseY, int windowW, int windowH) {
    // Only live when the model actually has clips (else the top-right corner
    // stays a normal scene-rotate region).
    if (!g_hasAnimations.load()) return false;
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        AnimBtnXFraction(), BTN_BAR_Y_FRACTION,
        ANIM_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

// Atlas capture is runtime-owned via xrCaptureAtlasEXT (XR_EXT_atlas_capture).
// App-side helpers (filename numbering + flash overlay) live in
// common/atlas_capture* — see dxr_capture::MakeCaptureAtlasPrefix /
// TriggerCaptureFlash / PostFlashRequest.

// Load a scene at startup. With an explicit path (first CLI arg) load that;
// otherwise fall back to the bundled avatar.fbx next to the exe.
static void TryAutoLoadBundledScene(const std::string& overridePath = std::string()) {
    std::string path;
    if (!overridePath.empty()) {
        if (!model_validate_file(overridePath)) {
            LOG_WARN("CLI model '%s' invalid/missing — falling back to bundled sample",
                     overridePath.c_str());
        } else {
            path = overridePath;
        }
    }
    if (path.empty()) {
        char exePath[MAX_PATH] = {0};
        if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return;
        // Strip basename
        char *lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) lastSlash = strrchr(exePath, '/');
        if (!lastSlash) return;
        *(lastSlash + 1) = '\0';
        path = std::string(exePath) + "avatar.fbx";
        if (!PathFileExistsA(path.c_str())) {
            LOG_INFO("No bundled scene at %s (skipping auto-load)", path.c_str());
            return;
        }
        if (!model_validate_file(path)) return;
    }
    LOG_INFO("Auto-loading scene: %s", path.c_str());
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    if (g_modelRenderer.loadModel(path.c_str())) {
        g_loadedFileName = model_basename(path);
        LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(), model_filesize_str(path).c_str());
        ApplyAutoFitForLoadedScene_locked();
    } else {
        LOG_WARN("Auto-load failed for %s", path.c_str());
    }
}

// Hand a picked path off to the render thread for scene load. Validates the
// extension first; on failure pops a MessageBox and returns false. Used by
// both the Win32 GetOpenFileNameA path and the #228 spatial picker result
// drained in the main loop.
static bool QueueSceneLoad(HWND hwnd, const std::string& path) {
    if (!model_validate_file(path)) {
        MessageBoxA(hwnd, "Invalid model file. Supported formats: .glb, .gltf", "Load Error", MB_OK | MB_ICONERROR);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
        g_pendingLoadPath = path;
    }
    g_loadRequested.store(true, std::memory_order_release);
    LOG_INFO("Queued model load: %s", path.c_str());
    return true;
}

// Open a file dialog and load a .ply or .spz scene (called from main thread).
//
// Path A — workspace + Tier 1 picker available:
//     xrRequestFilePickerEXT fires async. The completion event is drained
//     by PollEvents (common/xr_session_common.cpp) into xr.filePickerLast*;
//     the main loop dispatches to QueueSceneLoad on result arrival.
//
// Path B — workspace mode but no controller / no Tier 1 picker, OR running
// outside a workspace (standalone window), OR running on a non-DisplayXR
// OpenXR runtime: xrRequestFilePickerEXT either returns
// XR_FILE_PICKER_FALLBACK_TIER0_EXT (workspace fallback) or the PFN is
// null (extension absent). Either way fall through to GetOpenFileNameA
// and keep the existing standalone UX.
static void OpenLoadDialog(HWND hwnd) {
    // Already showing a spatial picker — second click on Open is a
    // no-op. Without this guard the prior "filePickerInFlight" check
    // would skip Path A and fall through to GetOpenFileNameA, opening
    // BOTH the spatial picker AND a flat Win32 dialog stacked on top.
    if (g_xr != nullptr && g_xr->filePickerInFlight) {
        LOG_INFO("[#228] OpenLoadDialog: spatial picker already in flight, ignoring");
        return;
    }

    // Path A: spatial picker, when available + not already in flight.
    if (g_xr != nullptr && g_xr->pfnRequestFilePickerEXT != nullptr &&
        !g_xr->filePickerInFlight) {
        XrFilePickerInfoEXT info = {XR_TYPE_FILE_PICKER_INFO_EXT};
        info.mode = XR_FILE_PICKER_MODE_OPEN_EXT;
        strncpy(info.title, "Load 3D Model",
                sizeof(info.title) - 1);
        info.filterCount = 3;
        strncpy(info.filters[0].description, "3D Models",
                sizeof(info.filters[0].description) - 1);
        strncpy(info.filters[0].extensions, "*.glb;*.gltf;*.stl;*.obj;*.fbx;*.usdz;*.usd;*.usda;*.usdc",
                sizeof(info.filters[0].extensions) - 1);
        strncpy(info.filters[1].description, "Binary glTF",
                sizeof(info.filters[1].description) - 1);
        strncpy(info.filters[1].extensions, "*.glb",
                sizeof(info.filters[1].extensions) - 1);
        strncpy(info.filters[2].description, "glTF",
                sizeof(info.filters[2].description) - 1);
        strncpy(info.filters[2].extensions, "*.gltf",
                sizeof(info.filters[2].extensions) - 1);

        XrAsyncRequestIdEXT rid = 0;
        XrResult r = g_xr->pfnRequestFilePickerEXT(g_xr->session, &info, &rid);
        if (r == XR_SUCCESS) {
            g_xr->filePickerInFlight = true;
            g_xr->filePickerRequestId = rid;
            LOG_INFO("[#228] xrRequestFilePickerEXT -> rc=0x%x requestId=%llu",
                r, (unsigned long long)rid);
            return; // wait for completion event in the main loop
        }
        // r == XR_FILE_PICKER_FALLBACK_TIER0_EXT or an error → fall through.
        LOG_INFO("[#228] xrRequestFilePickerEXT -> rc=0x%x (falling back to Win32)", r);
    }

    // Path B: existing Win32 file dialog (unchanged behavior).
    OPENFILENAMEA ofn = {};
    char filePath[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "3D Models (glb;gltf;stl;obj;fbx;usd*)\0*.glb;*.gltf;*.stl;*.obj;*.fbx;*.usdz;*.usd;*.usda;*.usdc\0glTF (*.glb;*.gltf)\0*.glb;*.gltf\0STL (*.stl)\0*.stl\0OBJ (*.obj)\0*.obj\0FBX (*.fbx)\0*.fbx\0USD (*.usd*)\0*.usdz;*.usd;*.usda;*.usdc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Load 3D Model";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        QueueSceneLoad(hwnd, std::string(filePath));
    }
}

// ── Per-pixel click-through silhouette ─────────────────────────────────────
// Each frame the render thread draws a mono, display-plane (single-eye) view of
// the avatar into a small scratch image (transparent bg → alpha≈1 on the
// avatar, 0 on the background) and publishes the alpha as a coverage bitmap.
// WM_NCHITTEST samples it: over the avatar → HTCLIENT (the app gets the click);
// off it → HTTRANSPARENT (the click falls through to the desktop window behind).
// Only consulted while borderless; when decorated (B) the normal frame stays
// fully interactive so the title bar can move/resize the window.
struct SilhouetteCoverage {
    std::mutex mtx;
    std::vector<uint8_t> bits;   // covW*covH, 1 = avatar present
    int covW = 0, covH = 0;
    int winW = 0, winH = 0;      // client size this coverage maps to
    bool ready = false;
};
static SilhouetteCoverage g_silCoverage;

// True if the client-space point is over the avatar silhouette (with a small
// dilation so hovering near edges is forgiving). Thread-safe; called on the UI
// thread from WM_NCHITTEST.
static bool SilhouetteHit(int clientX, int clientY) {
    std::lock_guard<std::mutex> lock(g_silCoverage.mtx);
    if (!g_silCoverage.ready || g_silCoverage.winW <= 0 || g_silCoverage.winH <= 0) return false;
    if (clientX < 0 || clientY < 0 || clientX >= g_silCoverage.winW || clientY >= g_silCoverage.winH) return false;
    const int sx = clientX * g_silCoverage.covW / g_silCoverage.winW;
    const int sy = clientY * g_silCoverage.covH / g_silCoverage.winH;
    const int R = 2;  // dilation radius in coverage pixels
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            const int x = sx + dx, y = sy + dy;
            if (x < 0 || y < 0 || x >= g_silCoverage.covW || y >= g_silCoverage.covH) continue;
            if (g_silCoverage.bits[(size_t)y * g_silCoverage.covW + x]) return true;
        }
    return false;
}

// Cross-process click-through via SetWindowRgn (the technique the working Unity
// plugin uses). WM_NCHITTEST/HTTRANSPARENT only forwards to same-thread windows,
// and a dynamically-toggled WS_EX_TRANSPARENT both fails to route cross-process
// reliably and stops the window getting mouse messages — both dead ends (the
// plugin tried and rejected them). SetWindowRgn instead clips the window to the
// avatar silhouette: OUTSIDE the region the OS treats the HWND as if it isn't
// there (for hit-testing AND rendering), so input falls through to whatever
// desktop window is behind — natively, cross-process. INSIDE the region the
// window catches input normally. We rebuild the region each tick from the
// published silhouette coverage. When decorated, the region is cleared so the
// whole framed window stays interactive for move/resize.
static const UINT_PTR kClickThroughTimerId = 0xC17;

static void UpdateClickRegion(HWND hwnd) {
    if (g_decorated) { SetWindowRgn(hwnd, NULL, TRUE); return; }

    std::vector<RECT> rects;
    int winW = 0, winH = 0;
    {
        std::lock_guard<std::mutex> lock(g_silCoverage.mtx);
        if (!g_silCoverage.ready || g_silCoverage.covW <= 0 || g_silCoverage.covH <= 0)
            return;
        const int cw = g_silCoverage.covW, ch = g_silCoverage.covH;
        winW = g_silCoverage.winW; winH = g_silCoverage.winH;
        // One client-space RECT per horizontal run of covered coverage-pixels.
        for (int y = 0; y < ch; ++y) {
            int x = 0;
            while (x < cw) {
                if (!g_silCoverage.bits[(size_t)y * cw + x]) { ++x; continue; }
                int xs = x;
                while (x < cw && g_silCoverage.bits[(size_t)y * cw + x]) ++x;
                RECT r;
                r.left   = xs * winW / cw;
                r.right  = x  * winW / cw;
                r.top    = y       * winH / ch;
                r.bottom = (y + 1) * winH / ch;
                rects.push_back(r);
            }
        }
    }

    if (rects.empty()) {
        // Nothing drawn → empty region = the whole window is click-through.
        SetWindowRgn(hwnd, CreateRectRgn(0, 0, 0, 0), TRUE);
        return;
    }

    // Pack the rects into one RGNDATA and build the region in a single call
    // (cheaper than CombineRgn per rect, and matches the plugin's approach).
    const size_t bytes = sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT);
    std::vector<uint8_t> buf(bytes);
    RGNDATA* rd = reinterpret_cast<RGNDATA*>(buf.data());
    rd->rdh.dwSize = sizeof(RGNDATAHEADER);
    rd->rdh.iType = RDH_RECTANGLES;
    rd->rdh.nCount = (DWORD)rects.size();
    rd->rdh.nRgnSize = (DWORD)(rects.size() * sizeof(RECT));
    RECT bb = rects[0];
    for (const RECT& r : rects) {
        if (r.left   < bb.left)   bb.left   = r.left;
        if (r.top    < bb.top)    bb.top    = r.top;
        if (r.right  > bb.right)  bb.right  = r.right;
        if (r.bottom > bb.bottom) bb.bottom = r.bottom;
    }
    rd->rdh.rcBound = bb;
    memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
    HRGN rgn = ExtCreateRegion(NULL, (DWORD)bytes, rd);
    SetWindowRgn(hwnd, rgn, TRUE);  // OS takes ownership of rgn
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_NCHITTEST: {
        // Borderless: the OS only delivers this when the cursor is INSIDE the
        // SetWindowRgn silhouette region (outside it the window is skipped
        // entirely and the event reaches the desktop), so just claim it.
        if (!g_decorated) return HTCLIENT;
        break;
    }
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        // UpdateInputState above already set leftButton/dragging=true. For
        // button clicks (which post a message to run a modal dialog or change
        // mode), clear that drag state — otherwise the modal eats the
        // matching WM_LBUTTONUP and subsequent mouse motion is interpreted as
        // a scene drag.
        if (IsClickOnLoadButton(mx, my, g_windowWidth, g_windowHeight)) {
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_inputState.leftButton = false;
                g_inputState.dragging = false;
            }
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        if (IsClickOnModeButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            // Mode button = cycle request (V-key equivalent). Main loop
            // reads runtime's current mode and computes the target.
            g_inputState.cycleRenderingModeRequested = true;
            return 0;
        }
        if (IsClickOnAnimButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            // Animation button = next clip (N-key equivalent).
            g_inputState.cycleClipRequested = true;
            return 0;
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_USER + 1:
        OpenLoadDialog(hwnd);
        return 0;

    case dxr_capture::kFlashUserMsg:
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == kClickThroughTimerId) {
            UpdateClickRegion(hwnd);
            return 0;
        }
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        // B key = toggle window decoration (borderless ⇄ title bar for move/resize)
        if (wParam == 'B') {
            ToggleDecoration(hwnd);
            return 0;
        }
        // L key = load shortcut
        if (wParam == 'L') {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        // I key = capture multi-view atlas
        if (wParam == 'I' || wParam == 'i') {
            g_captureAtlasRequested.store(true);
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null background brush + WS_EX_NOREDIRECTIONBITMAP (below) are required
    // by the runtime's transparent-window bridge (DComp + KMT shared texture).
    // Both must be set even when the demo defaults to opaque, because session
    // transparency is wired at xrCreateSession time and cannot be toggled later.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return nullptr;
        }
    }

    // Borderless by default (WS_POPUP): the avatar floats chrome-free over the
    // desktop. The B key toggles decoration back on for move/resize. Center the
    // client rect on the primary monitor so the borderless window isn't stranded
    // at (0,0) with no title bar to grab.
    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    {
        HMONITOR hMon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(hMon, &mi)) {
            const int monW = mi.rcWork.right - mi.rcWork.left;
            const int monH = mi.rcWork.bottom - mi.rcWork.top;
            posX = mi.rcWork.left + (monW - width) / 2;
            posY = mi.rcWork.top + (monH - height) / 2;
        }
    }

    // WS_EX_TOPMOST: the avatar always floats above other windows — when a
    // click passes through (HTTRANSPARENT) to a window behind, that window
    // activates but stays BELOW the avatar, so the tiger is never covered.
    HWND hwnd = CreateWindowEx(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST, WINDOW_CLASS, WINDOW_TITLE,
        WS_POPUP | WS_VISIBLE,
        posX, posY,
        width, height,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;
    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;
    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

// Render a simple "no scene" placeholder by clearing to dark gray
static void RenderPlaceholder(VkDevice device, VkQueue queue, VkCommandPool cmdPool,
                               VkImage image, uint32_t width, uint32_t height) {
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue clearColor = {{0.1f, 0.1f, 0.12f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    // Transition to COLOR_ATTACHMENT
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

// Scratch GPU image + host-visible readback for the silhouette pass (created
// lazily, resized with the window). Render-thread-owned.
static ModelImage  g_silImage = {};
static ModelBuffer g_silReadback = {};
static void*       g_silMapped = nullptr;

static bool EnsureSilhouetteTargets(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h) {
    if (g_silImage.image != VK_NULL_HANDLE && g_silImage.width == w && g_silImage.height == h)
        return true;
    if (g_silImage.image != VK_NULL_HANDLE) modelDestroyImage(dev, g_silImage);
    if (g_silReadback.buffer != VK_NULL_HANDLE) {
        if (g_silMapped) vkUnmapMemory(dev, g_silReadback.memory);
        modelDestroyBuffer(dev, g_silReadback);
        g_silMapped = nullptr;
    }
    g_silImage = modelCreateImage2D(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (g_silImage.image == VK_NULL_HANDLE) return false;
    g_silReadback = modelCreateBuffer(dev, phys, (VkDeviceSize)w * h * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (g_silReadback.buffer == VK_NULL_HANDLE) return false;
    vkMapMemory(dev, g_silReadback.memory, 0, (VkDeviceSize)w * h * 4, 0, &g_silMapped);
    return g_silMapped != nullptr;
}

// Render one mono eye of the avatar into the scratch image, read the alpha back,
// and publish the coverage bitmap consumed by WM_NCHITTEST. Cheap (downscaled to
// ~1/3 the window). Runs on the render thread right after the main eye render so
// it reuses the renderer's internal targets (no resize thrash — imageWidth/Height
// passed here match the main swapchain).
static void UpdateSilhouette(VkDevice dev, VkPhysicalDevice phys, VkQueue queue, VkCommandPool pool,
                            XrSessionManager* xr, uint32_t winW, uint32_t winH,
                            const float viewMat[16], const float projMat[16], float clipFar) {
    if (winW == 0 || winH == 0) return;
    uint32_t w = winW / 3; if (w < 64) w = 64; if (w > 640) w = 640;
    uint32_t h = winH / 3; if (h < 64) h = 64; if (h > 360) h = 360;
    if (!EnsureSilhouetteTargets(dev, phys, w, h)) return;

    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        if (!g_modelRenderer.hasModel()) return;
        g_modelRenderer.renderEye(g_silImage.image, VK_FORMAT_R8G8B8A8_UNORM,
            xr->swapchain.width, xr->swapchain.height,
            0, 0, w, h, viewMat, projMat, /*transparentBg=*/true, clipFar);
    }
    // renderEye leaves the scratch image in COLOR_ATTACHMENT_OPTIMAL → copy to host.
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier toSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = g_silImage.image;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    VkBufferImageCopy region = {};
    region.bufferRowLength = w;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, g_silImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        g_silReadback.buffer, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);

    const uint8_t* px = (const uint8_t*)g_silMapped;
    if (!px) return;
    std::lock_guard<std::mutex> lock(g_silCoverage.mtx);
    g_silCoverage.bits.resize((size_t)w * h);
    for (uint32_t i = 0; i < w * h; ++i) g_silCoverage.bits[i] = (px[i * 4 + 3] > 40) ? 1 : 0;
    g_silCoverage.covW = (int)w; g_silCoverage.covH = (int)h;
    g_silCoverage.winW = (int)winW; g_silCoverage.winH = (int)winH;
    g_silCoverage.ready = true;

    // Debug: with DXR_DUMP_SILHOUETTE set, dump the silhouette alpha (~once/sec)
    // to %TEMP%\avatar_silhouette.png so the hit mask can be eyeballed (white =
    // avatar, black = pass-through). Off by default.
    static const bool s_dumpSil = (GetEnvironmentVariableA("DXR_DUMP_SILHOUETTE", nullptr, 0) > 0);
    static int s_dbgCounter = 0;
    if (s_dumpSil && (s_dbgCounter++ % 60) == 0) {
        std::vector<uint8_t> a((size_t)w * h);
        for (uint32_t i = 0; i < w * h; ++i) a[i] = px[i * 4 + 3];
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        std::string path = std::string(tmp) + "avatar_silhouette.png";
        stbi_write_png(path.c_str(), (int)w, (int)h, 1, a.data(), (int)w);
    }
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkDevice vkDevice,
    VkQueue graphicsQueue,
    uint32_t queueFamilyIndex,
    VkInstance vkInstance,
    VkPhysicalDevice physDevice,
    std::vector<VkImage>* swapchainVkImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    VkBuffer hudStagingBuffer,
    void* hudStagingMapped,
    VkCommandPool hudCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages,
    VkCommandPool loadBtnCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* loadBtnSwapchainImages,
    uint32_t loadBtnWidth,
    uint32_t loadBtnHeight)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Command pool for placeholder rendering
    VkCommandPool renderCmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &renderCmdPool);
    }

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool animateToggle = false;
        bool loadReq = false;
        bool cycleModeRequested = false;
        bool cycleClip = false;
        bool playPause = false;
        int32_t absoluteModeRequest = -1;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
        }
        // Pitch sign flip for the GS demo: shared input_handler.cpp mutates
        // pitch with `-= dy` (cube_handle convention, paired with negative
        // VkViewport.height Y-flip at rasterization). The GS demo Y-flips
        // at the *view* stage in gs_renderer.cpp instead — that inverts the
        // visual response, so we negate here to restore drag-down → look-down.
        // Only `renderPitch` is used downstream; `inputSnapshot.pitch` itself
        // stays in shared-handler convention so the writeback at end of frame
        // doesn't compound.
        float renderPitch = -inputSnapshot.pitch;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            resetRequested = g_inputState.resetViewRequested;
            animateToggle = g_inputState.animateToggleRequested;
            loadReq = g_inputState.loadRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.teleportRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            cycleModeRequested = g_inputState.cycleRenderingModeRequested;
            g_inputState.cycleRenderingModeRequested = false;
            absoluteModeRequest = g_inputState.absoluteRenderingModeRequested;
            g_inputState.absoluteRenderingModeRequested = -1;
            g_inputState.eyeTrackingModeToggleRequested = false;
            if (g_inputState.transparentBgToggleRequested) {
                g_inputState.transparentBgToggleRequested = false;
                bool now = !g_transparentBg.load();
                g_transparentBg.store(now);
                LOG_INFO("Transparent background: %s (Ctrl+T)", now ? "ON" : "OFF");
            }
            g_inputState.animateToggleRequested = false;
            g_inputState.loadRequested = false;
            cycleClip = g_inputState.cycleClipRequested;
            g_inputState.cycleClipRequested = false;
            playPause = g_inputState.playPauseRequested;
            g_inputState.playPauseRequested = false;
            if (animateToggle) {
                g_inputState.animateEnabled = !g_inputState.animateEnabled;
                inputSnapshot.animateEnabled = g_inputState.animateEnabled;
            }
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // Request main thread to open file dialog when L key or Load button was pressed.
        if (loadReq) {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        }

        // Drain a queued scene load (set by OpenLoadDialog on the main
        // thread). We must run loadScene here because it submits Vulkan work
        // on the graphics queue, and that queue is exclusively driven by this
        // (render) thread for per-frame submissions — see g_pendingLoadPath.
        if (g_loadRequested.exchange(false, std::memory_order_acquire)) {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
                path = std::move(g_pendingLoadPath);
                g_pendingLoadPath.clear();
            }
            if (!path.empty()) {
                LOG_INFO("Loading model: %s", path.c_str());
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                if (g_modelRenderer.loadModel(path.c_str())) {
                    g_loadedFileName = model_basename(path);
                    LOG_INFO("Scene loaded: %s (%s)", g_loadedFileName.c_str(),
                        model_filesize_str(path).c_str());
                    ApplyAutoFitForLoadedScene_locked();
                } else {
                    LOG_ERROR("Failed to load scene: %s", path.c_str());
                    MessageBoxA(hwnd, "Failed to load scene file.\nThe file may be corrupt or unsupported.",
                        "Load Error", MB_OK | MB_ICONERROR);
                }
            }
        }

        // Rendering mode requests (V/mode-button=cycle, 0-8=absolute). Single
        // source of truth: runtime owns current mode via xr->currentModeIndex.
        if (cycleModeRequested && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE && xr->renderingModeCount > 0) {
            uint32_t next = (xr->currentModeIndex + 1) % xr->renderingModeCount;
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, next);
        }
        if (absoluteModeRequest >= 0 && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE &&
            (uint32_t)absoluteModeRequest < xr->renderingModeCount) {
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, (uint32_t)absoluteModeRequest);
        }

        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_EXT ? "MANUAL" : "MANAGED",
                    XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
            }
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);
        // Clip playback (N=next, K=play/pause). Render-thread only, like the
        // updateAnimation call below; apply before it so this frame reflects it.
        if (cycleClip) g_modelRenderer.cycleAnimation();
        if (playPause) g_modelRenderer.togglePaused();
        // Advance node/TRS animation once per frame (no-op for static models).
        g_modelRenderer.updateAnimation(perfStats.deltaTime);

        // On Space-reset: shared UpdateCameraMovement returns to (0,0,0) + default
        // vHeight. For the splat demo, restore the per-scene auto-fit pose instead.
        if (resetRequested && g_fitValid.load()) {
            inputSnapshot.cameraPosX = g_fitCenter[0];
            inputSnapshot.cameraPosY = g_fitCenter[1];
            inputSnapshot.cameraPosZ = g_fitCenter[2];
            inputSnapshot.yaw = g_fitYaw;
            inputSnapshot.viewParams.virtualDisplayHeight = g_fitVHeight;
        }

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            // Pose slerp and auto-orbit mutate yaw/pitch each frame — copy back.
            g_inputState.yaw = inputSnapshot.yaw;
            g_inputState.pitch = inputSnapshot.pitch;
            g_inputState.transitioning = inputSnapshot.transitioning;
            g_inputState.transitionT = inputSnapshot.transitionT;
            g_inputState.animationActive = inputSnapshot.animationActive;
            if (resetRequested) {
                g_inputState.viewParams = inputSnapshot.viewParams;
                // Auto-orbit stays OFF: the avatar's heading is driven by the
                // face-the-viewer billboard, not idle orbit.
                g_inputState.animateEnabled = false;
                g_inputState.transitioning = false;
            }
        }

        // Bind the virtual-display rig to a moving/skinned subject — but only in
        // X/Y. We center the avatar horizontally/vertically on the smoothed
        // skeleton centroid (so A/D/Q/E strafe and the animation's own drift
        // don't slide it off-screen), while leaving Z under the user's control.
        // That makes W/S the single depth lever: it dollies the avatar toward /
        // away from the viewer THROUGH the display-plane clip (foreground-only),
        // which is exactly the avatar UX (only W/S, clip at the screen plane).
        // Applied to inputSnapshot only, so g_inputState keeps the full pose.
        {
            float anchor[3];
            if (g_fitValid.load() && g_modelRenderer.getAnimatedAnchor(anchor)) {
                inputSnapshot.cameraPosX = anchor[0];
                inputSnapshot.cameraPosY = anchor[1];
                // cameraPosZ intentionally left as the user's W/S-driven value.
            }
        }

        PollEvents(*xr);

        // #228 Tier 1: drain a spatial-picker result if one arrived this
        // tick. PollEvents wrote the path + result code onto the session
        // manager; we route it through the same QueueSceneLoad path the
        // Win32 GetOpenFileNameA branch uses. The render thread picks the
        // queued path up via g_pendingLoadPath.
        if (xr->filePickerHasResult) {
            xr->filePickerHasResult = false;
            if (xr->filePickerLastResult == XR_FILE_PICKER_RESULT_SUCCESS_EXT &&
                xr->filePickerLastPath[0] != '\0') {
                QueueSceneLoad(hwnd, std::string(xr->filePickerLastPath));
            } else if (xr->filePickerLastResult == XR_FILE_PICKER_RESULT_CANCELLED_EXT) {
                LOG_INFO("[#228] User cancelled spatial picker — no scene load");
            } else {
                // PICKER_FAILED / INVALID_PATH — log and silently drop.
                // Don't auto-fall-back to Win32: the user already cancelled
                // out of the spatial flow, surfacing another dialog would
                // feel like a bug. They can click Load again if needed.
                LOG_WARN("[#228] Spatial picker delivered result=%d (no load)",
                    (int)xr->filePickerLastResult);
            }
        }

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                // Sized to runtime's max possible view count (sim_display Quad mode = 4).
                // Active mode's view count drives how many slots are actually filled and submitted.
                XrCompositionLayerProjectionView projectionViews[8] = {};
                bool rendered = false;
                bool hudSubmitted = false;
                bool loadBtnSubmitted = false;

                // Aspect-preserving HUD layer footprint (fixes demo-gs#8).
                // The HUD swapchain has a fixed pixel aspect (hudWidth × hudHeight,
                // sized once at session create). When the workspace tile is
                // resized to a different aspect, the runtime stretches the
                // swapchain per-axis to fit the layer rect — which distorts
                // glyphs and button shapes. Fix: pick layer-rect fractions
                // (layerFracW × layerFracH, in HWND fractions) that match the
                // swapchain aspect so both axes stretch by the same factor
                // (uniform scaling, no distortion). Same pattern as the runtime
                // test apps (test_apps/cube_handle_d3d11_win/main.cpp ~L800).
                // Prefer layerFracH = 1.0 (full window height, keeps the info
                // panel anchored to the window bottom); on extremely tall tiles
                // where that would push layerFracW past 1.0, clamp width and
                // shrink height instead.
                const float hudAR = (hudHeight > 0)
                    ? (float)hudWidth / (float)hudHeight : 1.0f;
                const float windowAR = (windowW > 0 && windowH > 0)
                    ? (float)windowW / (float)windowH : 1.0f;
                float layerFracH = 1.0f;
                float layerFracW = hudAR / windowAR;
                if (layerFracW > 1.0f) {
                    layerFracW = 1.0f;
                    layerFracH = windowAR / hudAR;
                }

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, renderPitch,
                        inputSnapshot.viewParams)) {

                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        // Over-allocate to the runtime's max possible view_count (sim_display
                        // reports 4 for Quad mode; LeiaSR reports 2). Hardcoding 2 here used
                        // to fail with XR_ERROR_SIZE_INSUFFICIENT under sim_display.
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[xr->currentModeIndex]);

                        // View count for the active rendering mode (1=mono, 2=stereo, 4=quad).
                        // Sized off the runtime's per-mode advertisement so the eye-loop and
                        // per-view buffers (rawEyes / stereoViews / viewMat / projectionViews)
                        // all line up with what xrEndFrame expects.
                        uint32_t activeViewCount = (xr->renderingModeCount > 0)
                            ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2u;
                        if (activeViewCount == 0) activeViewCount = 1u;
                        if (activeViewCount > 8) activeViewCount = 8u;
                        const int eyeCount = monoMode ? 1 : (int)activeViewCount;

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (cols × renderW, rows × renderH) are what gets written to
                        // the swapchain and snapshotted by the 'I' key. Swapchain
                        // creation already sized for the largest atlas, so no clamp.
                        // Falls back to the global recommendedViewScale (and 1.0 for
                        // mono) if the runtime didn't advertise per-mode info.
                        float scaleX, scaleY;
                        uint32_t cols, rows;
                        if (xr->renderingModeCount > 0) {
                            uint32_t mode = xr->currentModeIndex;
                            scaleX = xr->renderingModeScaleX[mode];
                            scaleY = xr->renderingModeScaleY[mode];
                            cols   = xr->renderingModeTileColumns[mode] ? xr->renderingModeTileColumns[mode] : 1u;
                            rows   = xr->renderingModeTileRows[mode]    ? xr->renderingModeTileRows[mode]    : 1u;
                        } else if (monoMode) {
                            scaleX = 1.0f; scaleY = 1.0f; cols = 1u; rows = 1u;
                        } else {
                            scaleX = xr->recommendedViewScaleX;
                            scaleY = xr->recommendedViewScaleY;
                            cols = 2u; rows = 1u;  // legacy SBS default
                        }
                        uint32_t renderW = (uint32_t)((double)windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;

                        // App-side Kooima projection (per-view, sized to active mode)
                        Display3DView stereoViews[8];
                        bool useAppProjection = (xr->hasDisplayInfoExt && xr->displayWidthM > 0.0f);
                        if (useAppProjection) {
                            float dispPxW = xr->displayPixelWidth > 0 ? (float)xr->displayPixelWidth : (float)xr->swapchain.width;
                            float dispPxH = xr->displayPixelHeight > 0 ? (float)xr->displayPixelHeight : (float)xr->swapchain.height;
                            float pxSizeX = xr->displayWidthM / dispPxW;
                            float pxSizeY = xr->displayHeightM / dispPxH;
                            float winW_m = (float)windowW * pxSizeX;
                            float winH_m = (float)windowH * pxSizeY;

                            // Window-relative Kooima: compute eye offset from window center
                            float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                            {
                                POINT clientOrigin = {0, 0};
                                ClientToScreen(hwnd, &clientOrigin);
                                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                                MONITORINFO mi = {sizeof(mi)};
                                if (GetMonitorInfo(hMon, &mi)) {
                                    float winCenterX = (float)(clientOrigin.x - mi.rcMonitor.left) + windowW / 2.0f;
                                    float winCenterY = (float)(clientOrigin.y - mi.rcMonitor.top) + windowH / 2.0f;
                                    float dispW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
                                    float dispH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
                                    eyeOffsetX = (winCenterX - dispW / 2.0f) * pxSizeX;
                                    eyeOffsetY = -((winCenterY - dispH / 2.0f) * pxSizeY);
                                }
                            }

                            // Build per-view raw eye positions in render frame.
                            // ModelRenderer::updateUniforms Y-mirrors the world; displayPose
                            // below is fed in render frame (cameraPosY negated). The
                            // rawEyes must live in the same render frame so the asymmetric
                            // Kooima projection's eye-vs-display geometry stays consistent
                            // — otherwise vertical eye parallax comes out inverted.
                            XrVector3f rawEyes[8];
                            if (monoMode) {
                                // Mono center = average of left/right raw views (0 and 1).
                                XrVector3f l = rawViews[0].pose.position;
                                XrVector3f r = rawViews[1].pose.position;
                                rawEyes[0].x = (l.x + r.x) * 0.5f - eyeOffsetX;
                                rawEyes[0].y = -((l.y + r.y) * 0.5f - eyeOffsetY);
                                rawEyes[0].z = (l.z + r.z) * 0.5f;
                            } else {
                                for (int i = 0; i < eyeCount; i++) {
                                    XrVector3f e = rawViews[i].pose.position;
                                    e.x -= eyeOffsetX;
                                    e.y -= eyeOffsetY;
                                    e.y = -e.y;
                                    rawEyes[i] = e;
                                }
                            }

                            // ── Face-the-viewer billboard (yaw + pitch) ────────
                            // Aim the avatar's forward axis along the line from the
                            // tiger to the viewer's head. rawEyes is the window-
                            // relative head position (it already has the eye minus
                            // the window-centre offset), so the heading updates BOTH
                            // as the viewer moves their head (eye tracking changes
                            // rawViews) AND as the window moves (the Kooima eyeOffset
                            // changes). Computed in the renderer's frame; the SIGN
                            // constants absorb the renderer Y-mirror — FACE_YAW_SIGN
                            // is validated (the viewer confirmed -1 turns the right
                            // way); FACE_PITCH_SIGN is the analogous vertical flip.
                            {
                                static constexpr float FACE_YAW_SIGN   = -1.0f;
                                static constexpr float FACE_PITCH_SIGN =  1.0f;  // viewer-confirmed
                                float hx, hy, hz;
                                if (eyeCount <= 1) {
                                    hx = rawEyes[0].x; hy = rawEyes[0].y; hz = rawEyes[0].z;
                                } else {
                                    hx = (rawEyes[0].x + rawEyes[1].x) * 0.5f;
                                    hy = (rawEyes[0].y + rawEyes[1].y) * 0.5f;
                                    hz = (rawEyes[0].z + rawEyes[1].z) * 0.5f;
                                }
                                // hz ≈ viewer distance in front of the display (~0.6 m).
                                const float hzAbs = fabsf(hz) > 1e-3f ? fabsf(hz) : 1e-3f;
                                const float horiz = sqrtf(hx * hx + hzAbs * hzAbs);
                                float targetYaw   = FACE_YAW_SIGN   * atan2f(hx, hzAbs);
                                float targetPitch = FACE_PITCH_SIGN * atan2f(hy, horiz);
                                // Exponential shortest-angle smoothing, TIME-BASED so
                                // it's frame-rate independent: a per-frame factor made
                                // the billboard stutter once the silhouette pass made
                                // frame timing uneven. A tight tau also keeps the yaw
                                // tracking the head as promptly as the motion-parallax
                                // projection, so the two don't visibly desync.
                                static float s_faceYaw = 0.0f, s_facePitch = 0.0f;
                                const float tau = 0.04f;  // seconds (settle ≈ 3·tau)
                                float a = 1.0f - expf(-perfStats.deltaTime / tau);
                                if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
                                // Only chase the head while eye tracking is LOCKED.
                                // During warmup (the first few seconds, isEyeTracking
                                // false) the tracker reports unstable positions that
                                // would jitter the heading — hold facing-forward
                                // (yaw/pitch 0) until it settles, then ease in.
                                if (xr->isEyeTracking) {
                                    float dy = targetYaw - s_faceYaw;
                                    while (dy >  3.14159265f) dy -= 6.28318531f;
                                    while (dy < -3.14159265f) dy += 6.28318531f;
                                    s_faceYaw   += dy * a;
                                    s_facePitch += (targetPitch - s_facePitch) * a;
                                }
                                // Override the render heading; auto-orbit + manual LMB
                                // are superseded by the face-the-viewer behaviour.
                                // displayPose below consumes inputSnapshot.yaw and the
                                // renderPitch local directly.
                                inputSnapshot.yaw = s_faceYaw;
                                // Pitch look-at disabled by request — yaw-only billboard
                                // (the avatar stays upright). FACE_PITCH_SIGN above is the
                                // viewer-confirmed sign; re-enable by uncommenting:
                                // renderPitch    = s_facePitch;
                                (void)s_facePitch;
                            }

                            Display3DTunables tunables;
                            tunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                            tunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                            tunables.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                            tunables.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                            // ZDP-relative clip: near/far are placed at fixed *absolute*
                            // offsets (in virtual-display-height units) in front of / behind
                            // the per-eye eye->display distance to the convergence plane
                            // (ZDP). display3d_compute_view anchors them per-eye from eye.z
                            // (near = ez - near_offset, far = ez + far_offset), so they scale
                            // with the virtual display (zoom). Transparent mode clips at the
                            // ZDP (far_offset = 0); opaque keeps a large recede band.
                            const float vH = tunables.virtual_display_height;
                            const float near_offset = vH;
                            const float far_offset  = g_transparentBg.load() ? 0.0f : 1000.0f * vH;

                            XrPosef displayPose;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                                renderPitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            displayPose.orientation = {q.x, q.y, q.z, q.w};
                            // ModelRenderer Y-mirrors the world inside updateUniforms (see comment
                            // in gs_renderer.cpp). The off-axis Kooima projection assumes the
                            // mirror is reflected in the displayPose passed to display3d_compute_views,
                            // so negate Y here to keep the eye-vs-display geometry consistent.
                            displayPose.position = {inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};

                            // nominalViewer in render frame too (Y mirrored) — used for
                            // parallax-factor lerp, must match the eye/displayPose frame.
                            XrVector3f nominalViewer = {xr->nominalViewerX, -xr->nominalViewerY, xr->nominalViewerZ};
                            Display3DScreen screen = {winW_m, winH_m};

                            display3d_compute_views(
                                rawEyes, (uint32_t)eyeCount, &nominalViewer,
                                &screen, &tunables, &displayPose,
                                near_offset, far_offset, /*vulkan_flip_y=*/0, stereoViews);
                            // displayxr::math emits a GL ([-1,1] clip-z) projection; this is a
                            // Vulkan renderer, so remap each per-view projection to [0,1]
                            // (reproduces modelviewer's prior direct-[0,1] output). [#396 W3]
                            for (uint32_t _v = 0; _v < (uint32_t)eyeCount; _v++)
                                convert_projection_gl_to_zero_to_one(stereoViews[_v].projection_matrix);
                        }

                        // Double-click focus: center-eye ray through mouse, pick splat,
                        // smoothly re-pose the virtual display to face back along the ray.
                        if (inputSnapshot.teleportRequested && useAppProjection) {
                            float ndcX = 2.0f * inputSnapshot.teleportMouseX / (float)windowW - 1.0f;
                            float ndcY = -(2.0f * inputSnapshot.teleportMouseY / (float)windowH - 1.0f);

                            float dispPxW2 = xr->displayPixelWidth > 0 ? (float)xr->displayPixelWidth : (float)xr->swapchain.width;
                            float dispPxH2 = xr->displayPixelHeight > 0 ? (float)xr->displayPixelHeight : (float)xr->swapchain.height;
                            float winW_m2 = (float)windowW * (xr->displayWidthM / dispPxW2);
                            float winH_m2 = (float)windowH * (xr->displayHeightM / dispPxH2);
                            Display3DScreen screen2 = {winW_m2, winH_m2};
                            Display3DTunables tunables2;
                            tunables2.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                            tunables2.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                            tunables2.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                            tunables2.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                            // Center-eye Display3DView from averaged processed display-space eye.
                            XrVector3f centerEyeDisp = {
                                (stereoViews[0].eye_display.x + stereoViews[1].eye_display.x) * 0.5f,
                                (stereoViews[0].eye_display.y + stereoViews[1].eye_display.y) * 0.5f,
                                (stereoViews[0].eye_display.z + stereoViews[1].eye_display.z) * 0.5f};
                            float m2v_post = tunables2.virtual_display_height / winH_m2;
                            float es = tunables2.perspective_factor * m2v_post;
                            XrVector3f centerEyeProcessed = (es != 0.0f)
                                ? XrVector3f{centerEyeDisp.x / es, centerEyeDisp.y / es, centerEyeDisp.z / es}
                                : centerEyeDisp;
                            // Use a real-world-frame displayPose for picking — the unproject
                            // ray needs to be in the same frame as the splats (un-Y-flipped
                            // world). The render-time displayPose has its Y negated for the
                            // renderer's view-stage Y mirror; using that here would put
                            // rayOrigin in a mirror frame and pickGaussian would intersect
                            // against splats in the wrong frame.
                            XrPosef displayPoseLocal;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(renderPitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            displayPoseLocal.orientation = {q.x, q.y, q.z, q.w};
                            displayPoseLocal.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                            Display3DView centerView;
                            const float pick_vH = tunables2.virtual_display_height;
                            const float pick_near_offset = pick_vH;
                            const float pick_far_offset  = g_transparentBg.load() ? 0.0f : 1000.0f * pick_vH;
                            display3d_compute_view(&centerEyeProcessed, &screen2, &tunables2,
                                                   &displayPoseLocal, pick_near_offset, pick_far_offset, &centerView);

                            XrVector3f rayOriginV, rayDirV;
                            display3d_unproject_ndc_to_ray(ndcX, ndcY,
                                centerView.view_matrix, centerView.projection_matrix,
                                &rayOriginV, &rayDirV);

                            float rayOrigin[3] = {rayOriginV.x, rayOriginV.y, rayOriginV.z};
                            float rayDir[3]    = {rayDirV.x,    rayDirV.y,    rayDirV.z};
                            float hitPos[3];
                            std::lock_guard<std::mutex> sceneLock(g_sceneMutex);
                            if (g_modelRenderer.pickSurface(rayOrigin, rayDir, hitPos)) {
                                // Both endpoints stored in WORLD frame (the same frame as
                                // inputSnapshot.cameraPosX/Y/Z and inputSnapshot.pitch/yaw)
                                // so the slerp's writeback decodes back into world-frame
                                // pitch/yaw. displayPoseLocal.orientation was built from
                                // renderPitch (-pitch) for the click-pick centerView; we
                                // must NOT reuse that render-frame quaternion here, else
                                // the slerp's `state.pitch = asin(fwd.y)` decode produces
                                // a sign-flipped pitch and the display rotates on teleport.
                                XMVECTOR worldOriQ = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMFLOAT4 wq;
                                XMStoreFloat4(&wq, worldOriQ);
                                XrQuaternionf worldOri = {wq.x, wq.y, wq.z, wq.w};

                                XrPosef fromWorld;
                                fromWorld.orientation = worldOri;
                                fromWorld.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                                XrPosef target;
                                target.position = {hitPos[0], hitPos[1], hitPos[2]};
                                target.orientation = worldOri;  // preserve current orientation — translate-only
                                std::lock_guard<std::mutex> inputLock(g_inputMutex);
                                g_inputState.transitionFrom = fromWorld;
                                g_inputState.transitionTo = target;
                                g_inputState.transitionT = 0.0f;
                                g_inputState.transitioning = true;
                                LOG_INFO("Focus on splat (%.3f, %.3f, %.3f)",
                                    hitPos[0], hitPos[1], hitPos[2]);
                            }
                        }

                        rendered = true;
                        // eyeCount already computed above from active mode's view count

                        // Mono center eye
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                            monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                            monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;

                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    renderPitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        // Foreground-only clip: in transparent mode, cull splats
                        // behind the virtual display plane so only popping-out
                        // content shows. Suppressed under the shell's external
                        // multi-compositor (non-controller workspace session,
                        // where the per-app transparent bridge is bypassed) —
                        // signalled by renderingModeIsRequestable being false.
                        bool standalone = (xr->renderingModeCount == 0) ||
                            (xr->currentModeIndex < xr->renderingModeCount &&
                             xr->renderingModeIsRequestable[xr->currentModeIndex]);
                        bool foregroundClip = g_transparentBg.load() && standalone;

                        // Build per-eye view/projection matrices (column-major float[16]).
                        // Sized to the runtime's max view count so Quad mode (4 views) fits.
                        float viewMat[8][16], projMat[8][16];
                        float clipFar[8] = {0};  // per-eye view-space far cull (0 = off)
                        for (int eye = 0; eye < eyeCount; eye++) {
                            if (useAppProjection) {
                                int srcEye = monoMode ? 0 : eye;
                                memcpy(viewMat[eye], stereoViews[srcEye].view_matrix, sizeof(float) * 16);
                                memcpy(projMat[eye], stereoViews[srcEye].projection_matrix, sizeof(float) * 16);
                                // eye_display.z = eye->display-plane forward distance,
                                // same world units as the shader's p_view.z.
                                if (foregroundClip) {
                                    float cf = stereoViews[srcEye].eye_display.z;
                                    clipFar[eye] = (cf > 0.2f) ? cf : 0.0f;  // never cull at/behind near
                                }
                            } else {
                                // Fallback: use DirectXMath mono matrices, store as column-major
                                XMMATRIX v = monoMode ? monoViewMatrix :
                                    XMMatrixLookAtRH(XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position),
                                        XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position) + XMVectorSet(0,0,-1,0),
                                        XMVectorSet(0,1,0,0));
                                XMMATRIX p = monoMode ? monoProjMatrix : xr->projMatrices[0];
                                // XMMatrix is row-major; transpose to get column-major for shader
                                XMMATRIX vT = XMMatrixTranspose(v);
                                XMMATRIX pT = XMMatrixTranspose(p);
                                XMStoreFloat4x4((XMFLOAT4X4*)viewMat[eye], vT);
                                XMStoreFloat4x4((XMFLOAT4X4*)projMat[eye], pT);
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            VkFormat colorFormat = (VkFormat)xr->swapchain.format;

                            bool hasGsScene;
                            {
                                std::lock_guard<std::mutex> lock(g_sceneMutex);
                                hasGsScene = g_modelRenderer.hasModel();
                            }

                            if (hasGsScene) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    // Row-major eye placement in the atlas; for 2×1 SBS
                                    // this is (0, renderW) at row 0; for mono (cols=1)
                                    // it collapses to (0, 0).
                                    uint32_t col = (uint32_t)eye % cols;
                                    uint32_t row = (uint32_t)eye / cols;
                                    uint32_t vpX = col * renderW;
                                    uint32_t vpY = row * renderH;
                                    g_modelRenderer.renderEye(
                                        (*swapchainVkImages)[imageIndex], colorFormat,
                                        xr->swapchain.width, xr->swapchain.height,
                                        vpX, vpY, renderW, renderH,
                                        viewMat[eye], projMat[eye],
                                        g_transparentBg.load(), clipFar[eye]);
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, renderCmdPool,
                                    (*swapchainVkImages)[imageIndex], xr->swapchain.width, xr->swapchain.height);
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasEXT
                            // (XR_EXT_atlas_capture, W6 of #396). The runtime owns
                            // the readback — no app-side staging texture. Works for
                            // any multi-view layout the runtime advertises; skipped
                            // for mono (1×1). Filename auto-increments. The prefix
                            // has no ".png"; the runtime appends "_atlas.png".
                            if (g_captureAtlasRequested.exchange(false)) {
                                if (!hasGsScene) {
                                    LOG_WARN("Capture skipped: no model loaded");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                } else if (xr->pfnCaptureAtlasEXT &&
                                           xr->session != XR_NULL_HANDLE) {
                                    std::string sceneName;
                                    {
                                        std::lock_guard<std::mutex> lock(g_sceneMutex);
                                        sceneName = g_loadedFileName;
                                    }
                                    // Strip extension from model filename
                                    // (e.g. "sample.glb" → "sample").
                                    auto dot = sceneName.find_last_of('.');
                                    std::string stem = (dot == std::string::npos)
                                        ? sceneName : sceneName.substr(0, dot);
                                    if (stem.empty()) stem = "scene";
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;
                                    strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);
                                    XrResult cr = xr->pfnCaptureAtlasEXT(xr->session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::PostFlashRequest(hwnd);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasEXT failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_EXT_atlas_capture not available");
                                }
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t col = (uint32_t)eye % cols;
                                uint32_t row = (uint32_t)eye / cols;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(col * renderW), (int32_t)(row * renderH)};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? rawViews[0].fov : rawViews[eye].fov);
                            }
                            ReleaseSwapchainImage(*xr);

                            // Update the click-through silhouette from the same
                            // left-eye matrices we just drew (so the hit mask
                            // tracks the animation, W/S dolly, billboard yaw, and
                            // the display-plane clip). Render-thread-local. Throttled
                            // to every other frame: the pass adds two GPU waits, and
                            // ~30 Hz is plenty for the hit region — keeping it off the
                            // every-frame path steadies frame timing (no billboard
                            // stutter).
                            static uint32_t s_silFrame = 0;
                            if (hasGsScene && (s_silFrame++ & 1u) == 0u)
                                UpdateSilhouette(vkDevice, physDevice, graphicsQueue, renderCmdPool,
                                    xr, windowW, windowH, viewMat[0], projMat[0], clipFar[0]);
                        } else {
                            rendered = false;
                        }

                        // Render the HUD info-panel window-space layer. Body-only
                        // now (chrome buttons moved to the top-bar layer below).
                        // The TAB toggle hides the body via the `drawBody` flag;
                        // the layer footprint stays the aspect-locked left strip.
                        // Only render/acquire the HUD swapchain when the panel is
                        // visible — when hidden the layer is dropped entirely (true
                        // toggle), so we must NOT acquire its image this frame.
                        if (rendered && hud && xr->hasHudSwapchain && hudSwapchainImages &&
                            inputSnapshot.hudVisible) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (Vulkan + glTF)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE";

                                // Scene info
                                std::wstring sceneText = L"\n--- Model ---";
                                {
                                    std::lock_guard<std::mutex> lock(g_sceneMutex);
                                    if (g_modelRenderer.hasModel()) {
                                        std::wstring fname(g_loadedFileName.begin(), g_loadedFileName.end());
                                        sceneText += L"\nLoaded: " + fname;
                                    } else {
                                        sceneText += L"\nNo scene loaded (press L or click Load)";
                                    }
                                }
                                modeText += sceneText;

                                // Per-view extent for HUD display — same formula as the
                                // render path (window × view_scale of the current mode).
                                float dispScaleX, dispScaleY;
                                if (xr->renderingModeCount > 0) {
                                    uint32_t mode = xr->currentModeIndex;
                                    dispScaleX = xr->renderingModeScaleX[mode];
                                    dispScaleY = xr->renderingModeScaleY[mode];
                                } else {
                                    dispScaleX = xr->recommendedViewScaleX;
                                    dispScaleY = xr->recommendedViewScaleY;
                                }
                                uint32_t dispRenderW = (uint32_t)((double)windowW * dispScaleX);
                                uint32_t dispRenderH = (uint32_t)((double)windowH * dispScaleY);
                                if (dispRenderW == 0) dispRenderW = 1;
                                if (dispRenderH == 0) dispRenderH = 1;
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(xr->currentModeIndex, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeNames[xr->currentModeIndex] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[xr->currentModeIndex] : true,
                                    xr->renderingModeCount > 0 ? xr->renderingModeIsRequestable[xr->currentModeIndex] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(renderPitch);
                                float fwdY =  sinf(renderPitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(renderPitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                float hudM2v = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    inputSnapshot.viewParams.perspectiveFactor, inputSnapshot.viewParams.scaleFactor);
                                {
                                    wchar_t vhBuf[96];
                                    int depthPct = (int)(inputSnapshot.viewParams.ipdFactor * 100.0f + 0.5f);
                                    const wchar_t* orbitLbl = inputSnapshot.animateEnabled
                                        ? (inputSnapshot.animationActive ? L"ON (running)" : L"ON (idle countdown)")
                                        : L"OFF";
                                    swprintf(vhBuf, 96, L"\nvHeight: %.3f  m2v: %.3f\nDepth/IPD: %d%%  Auto-Orbit: %s",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v, depthPct, orbitLbl);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = L"[WASDEQ] Move | [LMB-drag] Rotate | [Scroll] Zoom\n"
                                    L"[DblClick] Focus | [-/=] Depth | [Space] Reset | [N] Clip | [K] Play/Pause\n"
                                    L"[M] Auto-Orbit | [V] Mode | [L] Load | [Tab] HUD | [ESC] Quit";

                                // Chrome buttons no longer live here — they are a
                                // separate full-width top-bar window-space layer
                                // (see the button-bar block below). This layer is
                                // the info panel only, toggled by Tab via drawBody.
                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText, {},
                                    /*drawBody=*/true,
                                    /*bodyAtBottom=*/true);
                                if (pixels) {
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    uint8_t* dst = (uint8_t*)hudStagingMapped;
                                    for (uint32_t row = 0; row < hudHeight; row++) {
                                        memcpy(dst + row * hudWidth * 4, src + row * srcRowPitch, hudWidth * 4);
                                    }
                                    UnmapHud(*hud);
                                }

                                // Copy staging buffer to HUD swapchain image
                                VkCommandBufferAllocateInfo cmdAllocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                cmdAllocInfo.commandPool = hudCmdPool;
                                cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                cmdAllocInfo.commandBufferCount = 1;

                                VkCommandBuffer cmdBuf;
                                vkAllocateCommandBuffers(vkDevice, &cmdAllocInfo, &cmdBuf);

                                VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                vkBeginCommandBuffer(cmdBuf, &beginInfo);

                                VkImage hudImg = (*hudSwapchainImages)[hudImageIndex].image;

                                VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                                barrier.srcAccessMask = 0;
                                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.image = hudImg;
                                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                VkBufferImageCopy region = {};
                                region.bufferRowLength = hudWidth;
                                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                region.imageOffset = {0, 0, 0};
                                region.imageExtent = {hudWidth, hudHeight, 1};
                                vkCmdCopyBufferToImage(cmdBuf, hudStagingBuffer, hudImg,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                vkEndCommandBuffer(cmdBuf);

                                VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                                submitInfo.commandBufferCount = 1;
                                submitInfo.pCommandBuffers = &cmdBuf;
                                vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                                vkQueueWaitIdle(graphicsQueue);

                                vkFreeCommandBuffers(vkDevice, hudCmdPool, 1, &cmdBuf);

                                ReleaseHudSwapchainImage(*xr);
                                hudSubmitted = true;
                            }
                        }

                    }
                }

                // ── Top button bar: ONE full-width window-space layer holding all
                //    chrome buttons — Open + Mode packed left, Animation pinned
                //    right, transparent center. Always submitted (decoupled from
                //    the Tab-toggled HUD panel); the Animation pill is only added
                //    when the model has clips. Reuses the window-space-layer
                //    machinery (own swapchain / text renderer / staging) widened
                //    to a bar — see runtime issue #389. ──
                XrCompositionLayerWindowSpaceEXT barLayer = {};
                bool barLayerReady = false;
                if (g_animBtnReady && g_hasAnimBtnSwapchain) {
                    const float mxf = (g_windowWidth > 0)
                        ? (float)inputSnapshot.mouseX / (float)g_windowWidth : 0.0f;
                    const float myf = (g_windowHeight > 0)
                        ? (float)inputSnapshot.mouseY / (float)g_windowHeight : 0.0f;
                    const float barY = BTN_BAR_Y_FRACTION;
                    const float barH = BtnBarHeightFraction(windowW, windowH);
                    // Bar layer spans the full window width, so a button at
                    // window-x-fraction xf maps straight onto bar-texture-x. Pills
                    // fill ~70% of the bar height, vertically centered.
                    const float pillY = (float)BTN_BAR_TEX_H * 0.15f;
                    const float pillH = (float)BTN_BAR_TEX_H * 0.70f;
                    auto makeBtn = [&](float xf, float wf, const std::wstring& label) {
                        HudButton b;
                        b.label = label;
                        b.x = xf * (float)BTN_BAR_TEX_W;
                        b.y = pillY;
                        b.width = wf * (float)BTN_BAR_TEX_W;
                        b.height = pillH;
                        b.hovered = (mxf >= xf && mxf <= xf + wf &&
                                     myf >= barY && myf <= barY + barH);
                        return b;
                    };
                    std::vector<HudButton> barButtons;
                    barButtons.push_back(makeBtn(OPEN_BTN_X_FRACTION, OPEN_BTN_WIDTH_FRACTION, L"Open…"));
                    std::wstring modeLabel = L"Mode";
                    if (xr->renderingModeCount > 0 &&
                        xr->currentModeIndex < xr->renderingModeCount &&
                        xr->renderingModeNames[xr->currentModeIndex]) {
                        const char* nm = xr->renderingModeNames[xr->currentModeIndex];
                        modeLabel = L"Mode: " + std::wstring(nm, nm + strlen(nm));
                    }
                    // Surface workspace mode-lock so the user knows clicking Mode
                    // is a no-op in a locked workspace.
                    if (xr->renderingModeCount > 0 &&
                        xr->currentModeIndex < xr->renderingModeCount &&
                        !xr->renderingModeIsRequestable[xr->currentModeIndex]) {
                        modeLabel += L" [locked]";
                    }
                    barButtons.push_back(makeBtn(MODE_BTN_X_FRACTION, MODE_BTN_WIDTH_FRACTION, modeLabel));
                    if (g_hasAnimations.load()) {
                        std::wstring animLabel = L"Anim";
                        {
                            std::string clip; int ci, cn; float ct, cd; bool playing;
                            std::lock_guard<std::mutex> lk(g_sceneMutex);
                            if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing))
                                animLabel = playing ? std::wstring(clip.begin(), clip.end()) : L"Paused";
                        }
                        barButtons.push_back(makeBtn(AnimBtnXFraction(), ANIM_BTN_WIDTH_FRACTION, animLabel));
                    }

                    uint32_t pitch = 0;
                    const void* px = RenderHudAndMap(g_animBtnHud, &pitch,
                        L"", L"", L"", L"", L"", L"", L"", L"",
                        barButtons, /*drawBody=*/false, /*bodyAtBottom=*/true);
                    uint32_t idx = 0;
                    if (px && AcquireWindowSpaceImage(g_animBtnSwapchain, idx)) {
                        uint8_t* dst = (uint8_t*)g_animBtnStagingMapped;
                        for (uint32_t row = 0; row < BTN_BAR_TEX_H; ++row)
                            memcpy(dst + row * BTN_BAR_TEX_W * 4,
                                   (const uint8_t*)px + row * pitch, BTN_BAR_TEX_W * 4);
                        UnmapHud(g_animBtnHud);

                        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                        cai.commandPool = g_animBtnCmdPool;
                        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                        cai.commandBufferCount = 1;
                        VkCommandBuffer cb = VK_NULL_HANDLE;
                        vkAllocateCommandBuffers(vkDevice, &cai, &cb);
                        VkCommandBufferBeginInfo bgi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                        bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                        vkBeginCommandBuffer(cb, &bgi);
                        VkImage img = g_animBtnSwapImages[idx].image;
                        VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        bar.image = img;
                        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
                        VkBufferImageCopy rg = {};
                        rg.bufferRowLength = BTN_BAR_TEX_W;
                        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        rg.imageOffset = {0, 0, 0};
                        rg.imageExtent = {BTN_BAR_TEX_W, BTN_BAR_TEX_H, 1};
                        vkCmdCopyBufferToImage(cb, g_animBtnStaging, img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
                        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
                        vkEndCommandBuffer(cb);
                        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                        vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
                        vkQueueWaitIdle(graphicsQueue);
                        vkFreeCommandBuffers(vkDevice, g_animBtnCmdPool, 1, &cb);
                        ReleaseWindowSpaceImage(g_animBtnSwapchain);

                        barLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
                        barLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        barLayer.subImage.swapchain = g_animBtnSwapchain.swapchain;
                        barLayer.subImage.imageRect.offset = {0, 0};
                        barLayer.subImage.imageRect.extent = {(int32_t)BTN_BAR_TEX_W, (int32_t)BTN_BAR_TEX_H};
                        barLayer.subImage.imageArrayIndex = 0;
                        barLayer.x = 0.0f;
                        barLayer.y = BTN_BAR_Y_FRACTION;
                        barLayer.width = 1.0f;
                        barLayer.height = barH;
                        barLayer.disparity = 0.0f;
                        barLayerReady = true;
                    } else if (px) {
                        UnmapHud(g_animBtnHud);
                    }
                }

                // Submit frame
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                if (submitViewCount == 0) submitViewCount = 1;
                if (submitViewCount > 8) submitViewCount = 8;  // matches projectionViews[8] sizing
                if (rendered) {
                    // Always go through the window-space-layers path so the top
                    // button bar (an extra layer) shows. The HUD info-panel layer
                    // is gated by `submitHud = hudSubmitted`: when the panel is
                    // toggled off it was never rendered/acquired this frame, so we
                    // drop it entirely (true toggle, not a transparent layer).
                    // SOURCE_ALPHA on the projection layer: displayxr::common
                    // defaults projectionLayerFlags to 0, so pass the bit
                    // explicitly (the vendored copy hardcoded it; required for
                    // the Ctrl+T transparent-background path).
                    EndFrameWithWindowSpaceLayers(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, layerFracW, layerFracH, 0.0f, submitViewCount,
                        barLayerReady ? &barLayer : nullptr, barLayerReady ? 1u : 0u,
                        0, 0, -1, -1, /*submitHud=*/hudSubmitted,
                        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    // Silhouette scratch resources (render-thread-owned).
    if (g_silReadback.buffer != VK_NULL_HANDLE) {
        if (g_silMapped) vkUnmapMemory(vkDevice, g_silReadback.memory);
        modelDestroyBuffer(vkDevice, g_silReadback);
        g_silMapped = nullptr;
    }
    if (g_silImage.image != VK_NULL_HANDLE)
        modelDestroyImage(vkDevice, g_silImage);

    if (renderCmdPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vkDevice, renderCmdPool, nullptr);

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

// Global crash handler
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    const char* excName = "UNKNOWN";
    switch (exInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:      excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    excName = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         excName = "IN_PAGE_ERROR"; break;
        case EXCEPTION_GUARD_PAGE:            excName = "GUARD_PAGE"; break;
    }
    LOG_ERROR("!!! UNHANDLED EXCEPTION: %s (0x%08X) at address 0x%p !!!",
        excName, exInfo->ExceptionRecord->ExceptionCode,
        exInfo->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    // Optional CLI: a single .glb/.gltf path to load at startup instead of the
    // bundled sample. Trim surrounding whitespace + quotes from the raw cmdline.
    std::string cliModelPath;
    if (lpCmdLine && *lpCmdLine) {
        std::string s(lpCmdLine);
        size_t a = s.find_first_not_of(" \t\"");
        size_t b = s.find_last_not_of(" \t\"");
        if (a != std::string::npos) cliModelPath = s.substr(a, b - a + 1);
    }

    SetUnhandledExceptionFilter(CrashHandler);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== DisplayXR 3D Avatar (Vulkan) ===");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Initialize OpenXR
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Try to load sim_display_set_output_mode
    {
        HMODULE rtModule = GetModuleHandleA("openxr_displayxr.dll");
        if (!rtModule) rtModule = GetModuleHandleA("openxr_displayxr");
        if (rtModule) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)GetProcAddress(rtModule, "sim_display_set_output_mode");
        }
        LOG_INFO("sim_display output mode: %s", g_pfnSetOutputMode ? "available" : "not available");
    }

    // Get Vulkan graphics requirements
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, physDevice, deviceExtensions, extensionStorage)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family found");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);

        // Extract VkImage handles for render thread access
    }
    std::vector<VkImage> swapchainVkImages(swapchainImages.size());
    for (uint32_t i = 0; i < (uint32_t)swapchainImages.size(); i++) {
        swapchainVkImages[i] = swapchainImages[i].image;
    }

    // Initialize model renderer with the OpenXR Vulkan device
    {
        uint32_t renderW = xr.swapchain.width;   // Full width — mono uses entire swapchain
        uint32_t renderH = xr.swapchain.height;
        if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                               queueFamilyIndex, renderW, renderH)) {
            LOG_WARN("model renderer init failed - scene rendering will not be available");
        } else {
            TryAutoLoadBundledScene(cliModelPath);
        }
    }

    // Initialize HUD renderer
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    uint32_t hudFontBaseHeight = (uint32_t)(xr.swapchain.height * HUD_FONT_BASE_FRACTION);
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight, hudFontBaseHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain
    std::vector<XrSwapchainImageVulkanKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u Vulkan images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create HUD staging buffer
    VkBuffer hudStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hudStagingMemory = VK_NULL_HANDLE;
    void* hudStagingMapped = nullptr;
    VkCommandPool hudCmdPool = VK_NULL_HANDLE;

    if (hudOk) {
        VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = (VkDeviceSize)hudWidth * hudHeight * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &hudStagingBuffer) != VK_SUCCESS) {
            LOG_WARN("Failed to create HUD staging buffer");
            hudOk = false;
        }

        if (hudOk) {
            VkMemoryRequirements memReqs;
            vkGetBufferMemoryRequirements(vkDevice, hudStagingBuffer, &memReqs);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags &
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }

            if (memTypeIndex == UINT32_MAX) {
                LOG_WARN("No suitable memory type for HUD staging buffer");
                hudOk = false;
            } else {
                VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = memTypeIndex;
                vkAllocateMemory(vkDevice, &allocInfo, nullptr, &hudStagingMemory);
                vkBindBufferMemory(vkDevice, hudStagingBuffer, hudStagingMemory, 0);
                vkMapMemory(vkDevice, hudStagingMemory, 0, bufInfo.size, 0, &hudStagingMapped);
            }
        }

        if (hudOk) {
            VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &hudCmdPool) != VK_SUCCESS) {
                LOG_WARN("Failed to create HUD command pool");
                hudOk = false;
            }
        }

        if (hudOk) {
            LOG_INFO("HUD Vulkan resources created (%ux%u)", hudWidth, hudHeight);
        }
    }

    // ── Top button-bar window-space layer resources ──────────────────────────
    // Own swapchain + text renderer + staging + cmd pool for the full-width top
    // button bar (Open + Mode + Animation in one layer). Reuses the
    // g_animBtnSwapchain / g_animBtn* slots (named before the buttons were
    // unified into a bar). Only when window-space layers are available.
    if (hudOk && xr.hasHudSwapchain) {
        if (InitializeHudRenderer(g_animBtnHud, BTN_BAR_TEX_W, BTN_BAR_TEX_H, BTN_BAR_FONT_BASE) &&
            CreateWindowSpaceSwapchain(xr, g_animBtnSwapchain, BTN_BAR_TEX_W, BTN_BAR_TEX_H)) {
            g_hasAnimBtnSwapchain = true;
            uint32_t c = g_animBtnSwapchain.imageCount;
            g_animBtnSwapImages.resize(c, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(g_animBtnSwapchain.swapchain, c, &c,
                (XrSwapchainImageBaseHeader*)g_animBtnSwapImages.data());

            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = (VkDeviceSize)BTN_BAR_TEX_W * BTN_BAR_TEX_H * 4;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            bool ok = vkCreateBuffer(vkDevice, &bi, nullptr, &g_animBtnStaging) == VK_SUCCESS;
            if (ok) {
                VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vkDevice, g_animBtnStaging, &mr);
                VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physDevice, &mp);
                uint32_t mt = UINT32_MAX;
                for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (mp.memoryTypes[i].propertyFlags &
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
                if (mt != UINT32_MAX) {
                    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
                    vkAllocateMemory(vkDevice, &ai, nullptr, &g_animBtnStagingMem);
                    vkBindBufferMemory(vkDevice, g_animBtnStaging, g_animBtnStagingMem, 0);
                    vkMapMemory(vkDevice, g_animBtnStagingMem, 0, bi.size, 0, &g_animBtnStagingMapped);
                } else ok = false;
            }
            if (ok) {
                VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pci.queueFamilyIndex = queueFamilyIndex;
                ok = vkCreateCommandPool(vkDevice, &pci, nullptr, &g_animBtnCmdPool) == VK_SUCCESS;
            }
            g_animBtnReady = ok;
            LOG_INFO("Animation-button layer resources %s", ok ? "created" : "FAILED");
        } else {
            LOG_WARN("Animation-button layer init failed — button will not show");
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // ~60 Hz poll that toggles WS_EX_TRANSPARENT for shaped click-through
    // (cursor over the avatar → opaque to input; off it → pass-through).
    SetTimer(hwnd, kClickThroughTimerId, 16, nullptr);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom  DblClick=Focus");
    LOG_INFO("          -/= Depth  Space=Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          L=Load  Tab=HUD  F11=Fullscreen  ESC=Quit");
    LOG_INFO("");

    g_inputState.viewParams.virtualDisplayHeight = kFallbackVirtualDisplayHeightM;
    g_inputState.renderingModeCount = xr.renderingModeCount;
    // Align runtime active rendering mode with app's default (mode 1 = first 3D mode).
    // The main loop's dispatch picks this up on the first frame and calls
    // xrRequestDisplayRenderingModeEXT(1); the runtime event drives xr.currentModeIndex.
    g_inputState.absoluteRenderingModeRequested = 1;
    g_inputState.hudVisible = false;     // hidden by default; toggle with Tab
    g_inputState.animateEnabled = false; // no auto-orbit — avatar faces the viewer
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
    }

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, vkDevice, graphicsQueue,
        queueFamilyIndex, vkInstance, physDevice,
        &swapchainVkImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudStagingBuffer, hudStagingMapped, hudCmdPool,
        hudOk ? &hudSwapImages : nullptr,
        (VkCommandPool)VK_NULL_HANDLE, (std::vector<XrSwapchainImageVulkanKHR>*)nullptr,
        (uint32_t)0, (uint32_t)0);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    g_modelRenderer.cleanup();

    if (hudCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, hudCmdPool, nullptr);
    if (hudStagingBuffer != VK_NULL_HANDLE) {
        vkUnmapMemory(vkDevice, hudStagingMemory);
        vkDestroyBuffer(vkDevice, hudStagingBuffer, nullptr);
    }
    if (hudStagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, hudStagingMemory, nullptr);
    if (hudOk) CleanupHudRenderer(hudRenderer);

    // Animation-button layer resources.
    if (g_animBtnCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, g_animBtnCmdPool, nullptr);
    if (g_animBtnStaging != VK_NULL_HANDLE) {
        if (g_animBtnStagingMapped) vkUnmapMemory(vkDevice, g_animBtnStagingMem);
        vkDestroyBuffer(vkDevice, g_animBtnStaging, nullptr);
    }
    if (g_animBtnStagingMem != VK_NULL_HANDLE) vkFreeMemory(vkDevice, g_animBtnStagingMem, nullptr);
    if (g_animBtnReady) CleanupHudRenderer(g_animBtnHud);

    // App-owned animation-button swapchain: destroy before CleanupOpenXR tears
    // the session down (used to live in the vendored XrSessionManager cleanup).
    if (g_animBtnSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_animBtnSwapchain.swapchain);
        g_animBtnSwapchain.swapchain = XR_NULL_HANDLE;
        g_hasAnimBtnSwapchain = false;
    }

    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
