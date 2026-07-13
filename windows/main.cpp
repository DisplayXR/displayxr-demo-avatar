// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
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
#include <openxr/XR_DXR_local_3d_zone.h>  // XrCompositionLayerLocal2DDXR (speech bubble)
#include <openxr/XR_DXR_display_zones.h>  // XrDisplayZoneDXR — the tiger zone (ADR-027)
#include <openxr/XR_DXR_view_rig.h>       // XrDisplayRigDXR locate + XrViewDisplayRawDXR readback
#include "display3d_view.h"
#include "projection_depth.h"

#include "hud_renderer.h"   // HudRenderer + text_overlay (RenderFilledRect/RenderText) — drive the speech bubble
#include "atlas_capture.h"
#include <dwrite.h>
#pragma comment(lib, "dwrite.lib")

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>    // snprintf — MCP tool-result JSON
#include <cstdlib>   // strtod / strtof — MCP numeric args
#include <cstring>   // strcmp / strstr / strchr — MCP tool dispatch
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

// Speech-bubble swapchain texture. Generous + roughly 4:1 so the rounded panel
// can be rendered into an aspect-matched SUB-RECT of it (sized to the live top-25%
// band each frame) and mapped sub-rect→full-band with no stretch — see the bubble
// block in the render loop. Sized for resolution headroom across band aspects.
static const uint32_t BTN_BAR_TEX_W = 2048;
static const uint32_t BTN_BAR_TEX_H = 512;
static const uint32_t BTN_BAR_FONT_BASE = 256;  // HudRenderer init only; the bubble swaps its own format

// Speech-bubble greeting. Wrapped + balanced + centred at render time, re-fit to
// the panel whenever the window (hence the band sub-rect) is resized.
static const std::wstring g_bubbleText =
    L"Hi there! I'm Leo, your friendly 3D desktop avatar.";
// One font family/weight shared by the fit-measure and the render so the measured
// extents match what's drawn (a heavier render than measure would overflow wrap).
static const wchar_t*           kBubbleFont   = L"Segoe UI";
static const DWRITE_FONT_WEIGHT kBubbleWeight = DWRITE_FONT_WEIGHT_SEMI_BOLD;

// Largest font (px) whose word-wrapped `text` fits within maxW×maxH, then the
// smallest wrap width that still yields that many lines (balanced block).
// Returns the font px; fills outWrapW/outTextH with the balanced text extents.
static float ComputeBubbleFit(const std::wstring& text, float maxW, float maxH,
                              float& outWrapW, float& outTextH) {
    outWrapW = maxW; outTextH = maxH;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) return maxH * 0.5f;

    auto measure = [&](float fontPx, float wrapW, uint32_t& lines, float& w, float& h) -> bool {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dw->CreateTextFormat(kBubbleFont, nullptr, kBubbleWeight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontPx, L"en-us", &fmt)))
            return false;
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(),
                wrapW, 100000.0f, &layout))) return false;
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        lines = m.lineCount;
        w = m.widthIncludingTrailingWhitespace;
        h = m.height;
        return true;
    };

    // (1) Largest font that fits maxW × maxH (greedy wrap at maxW).
    float lo = 6.0f, hi = maxH, best = lo; uint32_t bestLines = 1;
    for (int i = 0; i < 22; ++i) {
        float mid = 0.5f * (lo + hi);
        uint32_t lines; float w, h;
        if (measure(mid, maxW, lines, w, h) && h <= maxH && w <= maxW) {
            best = mid; bestLines = lines; lo = mid;
        } else hi = mid;
    }

    // (2) Balance: smallest wrap width that still yields `bestLines` lines.
    float wlo = 1.0f, whi = maxW, balW = maxW;
    for (int i = 0; i < 22; ++i) {
        float midW = 0.5f * (wlo + whi);
        uint32_t lines; float w, h;
        if (measure(best, midW, lines, w, h) && lines <= bestLines && w <= midW + 1.0f && h <= maxH) {
            balW = midW; whi = midW;
        } else wlo = midW;
    }
    uint32_t lines; float w, h;
    if (measure(best, balW, lines, w, h)) { outWrapW = w; outTextH = h; }
    else { outWrapW = balW; outTextH = maxH; }
    return best;
}

// Render the speech bubble — a rounded glassy panel filling the top-left subW×subH
// sub-rect of the bubble texture, with the greeting wrapped balanced + centred
// inside — clearing the rest of the texture transparent. The layout fit (font px +
// balanced wrap width) is recomputed only when the sub-rect size changes (i.e. on
// resize), so the per-frame cost is just the D2D draw + map. Reuses the common
// text_overlay primitives (RenderFilledRect for the rounded panel, RenderText for
// the wrapped text — the bubble can't use RenderButton, which forces NO_WRAP).
// Returns the mapped staging pixels (R8G8B8A8) + row pitch; caller uploads then
// calls UnmapHud(). Mirrors RenderHudAndMap's copy/map tail.
static const void* RenderBubbleToTexture(HudRenderer& hud, uint32_t subW, uint32_t subH,
                                         const std::wstring& text, uint32_t* rowPitch) {
    if (subW < 2 || subH < 2) return nullptr;
    // ── Cached layout (recompute on sub-rect size change) ──
    static uint32_t s_lastW = 0, s_lastH = 0;
    static float s_panelX = 0, s_panelY = 0, s_panelW = 0, s_panelH = 0, s_radius = 0;
    static float s_textX = 0, s_textW = 0;
    if (subW != s_lastW || subH != s_lastH) {
        s_lastW = subW; s_lastH = subH;
        // Near-edge-to-edge panel (the band IS the bubble) — just enough margin for
        // the rounded corners + the desktop to peek through them.
        const float mX = subW * 0.010f, mY = subH * 0.020f;
        s_panelX = mX; s_panelY = mY;
        s_panelW = (float)subW - 2.0f * mX; s_panelH = (float)subH - 2.0f * mY;
        s_radius = s_panelH * 0.16f;
        const float padX = s_panelW * 0.06f, padY = s_panelH * 0.16f;
        float wrapW = s_panelW - 2.0f * padX, textH = s_panelH - 2.0f * padY;
        const float fontPx = ComputeBubbleFit(text, s_panelW - 2.0f * padX,
                                              s_panelH - 2.0f * padY, wrapW, textH);
        s_textW = wrapW;
        s_textX = s_panelX + (s_panelW - s_textW) * 0.5f;
        // Rebuild the render format at the fit font — centred + word-wrapping, so
        // RenderText (which uses overlay.smallTextFormat) draws the balanced block.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        if (SUCCEEDED(hud.overlay.dwriteFactory->CreateTextFormat(
                kBubbleFont, nullptr, kBubbleWeight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontPx, L"en-us", &fmt))) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            hud.overlay.smallTextFormat = fmt;
        }
        LOG_INFO("Bubble fit: sub=%ux%u font=%.0fpx wrapW=%.0f", subW, subH, fontPx, s_textW);
    }

    // ── Clear texture fully transparent (D3D11), then draw panel + text (D2D) ──
    ID3D11RenderTargetView* rtv = nullptr;
    hud.device->CreateRenderTargetView(hud.renderTex.Get(), nullptr, &rtv);
    if (rtv) { float c[4] = {0, 0, 0, 0}; hud.context->ClearRenderTargetView(rtv, c); rtv->Release(); }

    RenderFilledRect(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
                     s_panelX, s_panelY, s_panelW, s_panelH,
                     0.05f, 0.05f, 0.09f, 0.64f, s_radius);
    RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
               text, s_textX, s_panelY, s_textW, s_panelH, /*useSmallFont=*/true);

    // ── Copy to staging + map (mirror RenderHudAndMap's tail) ──
    hud.context->CopyResource(hud.stagingTex.Get(), hud.renderTex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(hud.context->Map(hud.stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return nullptr;
    if (rowPitch) *rowPitch = mapped.RowPitch;

    // Debug: with DXR_DUMP_BUBBLE set, dump the panel sub-rect to
    // %TEMP%\avatar_bubble.png (once) so the 2D bubble layout can be eyeballed
    // without the live display (the composed Local2D layer is otherwise
    // uncapturable from the in-process VK path). Premultiplied-alpha RGBA.
    static const bool s_dumpBubble = (GetEnvironmentVariableA("DXR_DUMP_BUBBLE", nullptr, 0) > 0);
    static bool s_dumped = false;
    if (s_dumpBubble && !s_dumped && mapped.pData) {
        s_dumped = true;
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        std::string path = std::string(tmp) + "avatar_bubble.png";
        stbi_write_png(path.c_str(), (int)subW, (int)subH, 4, mapped.pData, (int)mapped.RowPitch);
    }
    return mapped.pData;
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
// Portrait default: a standing full-body avatar wants a tall window (the
// bottom 75% is the tiger zone, top 25% the speech bubble). Physical pixels
// (the app is PerMonitorV2 DPI-aware); B toggles decoration for move/resize.
// Size = user-tuned on the 3840x2160 Leia panel (measured client area).
static UINT g_windowWidth = 811;
static UINT g_windowHeight = 1421;

// 3DGS state
static ModelRenderer g_modelRenderer;
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

// Speech-bubble window-space layer resources: created in main() (when the
// runtime advertises Local2D), used by the render thread. The swapchain is
// app-owned state (displayxr::common's XrSessionManager carries no app-named
// fields, #396 W4) — created via the lib's CreateWindowSpaceSwapchain generic,
// destroyed before CleanupOpenXR. The g_animBtn* slot names predate the strip
// of the old chrome buttons; they now drive only the bubble pill.
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
// Initial virtual-display height as a multiple of the avatar's height. The
// avatar should occupy 90% of the virtual-display (i.e. its bottom-75% canvas)
// height at start, so vHeight = modelHeight / 0.90 = modelHeight × 1.111 → 5%
// headroom top and bottom.
static constexpr float kAutoFitVerticalComfort = 1.111f;

// Cached auto-fit pose for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kFallbackVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static std::atomic<bool> g_fitValid{false};

// XR_DXR_mcp_tools (#30): agent-tool session state. `g_mcpToolsReady` is set
// once the appId + base tools are registered (RegisterMcpBaseTools, after
// xrCreateSession); `g_mcpAnimToolsRegistered` tracks whether the late-bound
// animation tools (list/play/stop_animation) are currently live. Both are
// touched only on the main thread at startup and on the render thread
// thereafter (never concurrently — the render thread starts after startup
// registration completes).
static bool g_mcpToolsReady = false;
static bool g_mcpAnimToolsRegistered = false;

// XR_DXR_mcp_tools (#30): (un)register the animation tools when a model with /
// without clips loads. Defined below; forward-declared so every load path
// (ApplyAutoFitForLoadedScene_locked) can flip the registration.
static void UpdateMcpAnimationTools();

// Compute robust scene bounds (5th–95th percentile per axis) and stage
// new display-rig pose + vHeight on g_inputState. Display orientation is
// kept identity (forward = world −Z): splats have no canonical front, and
// any heuristic (PCA, etc.) can pick the wrong side; the user can rotate
// with mouse drag from a predictable starting pose.
// Caller must hold g_sceneMutex (we read pickData_ from the renderer).
static void ApplyAutoFitForLoadedScene_locked() {
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

    // XR_DXR_mcp_tools (#30): the new model may or may not have clips, so
    // (un)register the agent animation tools accordingly. Inert until the base
    // tools are registered (g_mcpToolsReady) — at first auto-load the session
    // exists and the base tools are already up, so the tiger's clips surface
    // the tools immediately; late loads via the load_model tool refresh them.
    UpdateMcpAnimationTools();
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

// Atlas capture is runtime-owned via xrCaptureAtlasDXR (XR_DXR_atlas_capture).
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

// ============================================================================
// XR_DXR_mcp_tools (#30) — app-defined agent tools
// ============================================================================
// The avatar exposes its controls (model load, camera orbit, framing, status,
// animation playback) as MCP tools on the per-process server the runtime hosts.
// This is a faithful Windows port of the macOS implementation (macos/main.mm):
// same appId ("avatar"), same tool names, and byte-for-byte the same
// descriptions + JSON input schemas, wired to the Windows app's real state
// (g_modelRenderer + g_inputState). Registration is never load-bearing — when
// the MCP capability gate is off (or the runtime predates the extension) the
// PFNs are NULL and every path here is an inert no-op.
//
// NOTE ON THE EVENT LOOP: the Windows app uses displayxr-common's shared
// PollEvents unchanged. displayxr-common v2.1.0 (common #18) added an
// app-supplied mcpToolHandler hook: when set, PollEvents fetches the call args,
// invokes the handler, and submits the returned JSON (with the handler's
// success flag gating the ok bit) — so the avatar no longer has to fork
// PollEvents just to swap the cube reference dispatch for its own. The handler
// (HandleMcpToolCall below) is a pure (toolName, argsJson) -> resultJson map;
// it must NOT call xrGetMCPToolCallArgsDXR / xrSubmitMCPToolResultDXR (PollEvents
// does both). It runs on the render thread inside PollEvents and takes
// g_inputMutex / g_sceneMutex exactly like the keyboard + auto-load paths do.

// Minimal JSON helpers — hand-rolled on purpose, matching macos/main.mm and the
// runtime reference adopter: tool args are tiny one-level objects, so a JSON
// dependency isn't warranted.
static std::string McpJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract "key":"value" (string) with backslash-escape handling, incl. a basic
// \uXXXX → UTF-8 decode (no surrogate pairs — file paths don't need them).
// False when the key is absent or its value is not a string.
static bool McpJsonGetString(const char* json, const char* key, std::string& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (*c != '"') return false;
    c++;
    out.clear();
    while (*c && *c != '"') {
        if (*c == '\\' && c[1]) {
            c++;
            switch (*c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    int ndig = 0;
                    while (ndig < 4 && c[1]) {
                        char h = c[1];
                        unsigned v;
                        if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
                        else break;
                        cp = (cp << 4) | v;
                        c++;
                        ndig++;
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += *c; break;  // \" \\ \/
            }
        } else {
            out += *c;
        }
        c++;
    }
    return *c == '"';
}

// Extract "key": <number>. False when absent or not numeric (strtod refuses a
// leading quote, so string values correctly fail).
static bool McpJsonGetNumber(const char* json, const char* key, double& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    char* end = nullptr;
    double v = strtod(c + 1, &end);
    if (end == c + 1) return false;
    out = v;
    return true;
}

// ★ Late (un)registration of the animation tools (#30): they exist only while a
// model with clips is loaded, and disappear when replaced by one without. Each
// transition makes the runtime broadcast the MCP tools/list_changed
// notification, so agents connected before a load see them appear/disappear
// live. Called from every successful load path (all funnel through
// ApplyAutoFitForLoadedScene_locked). Runs under g_sceneMutex (the load holds
// it), so g_modelRenderer reads are safe.
static void UpdateMcpAnimationTools() {
    if (!g_mcpToolsReady || !g_pfnRegisterMCPTool || !g_pfnUnregisterMCPTool ||
        g_xr == nullptr || g_xr->session == XR_NULL_HANDLE) {
        return;
    }
    const bool want = g_modelRenderer.hasAnimations();
    if (want == g_mcpAnimToolsRegistered) return;

    if (want) {
        XrMCPToolInfoDXR listTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        listTool.name = "list_animations";
        listTool.description =
            "List the loaded model's animation clips: index, name and duration in "
            "seconds, plus the active clip index and whether playback is running. "
            "Only available while a model with animation clips is loaded.";
        listTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r1 = g_pfnRegisterMCPTool(g_xr->session, &listTool);

        XrMCPToolInfoDXR playTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        playTool.name = "play_animation";
        playTool.description =
            "Play an animation clip, selected by 'index' or 'name' (see "
            "list_animations). Omit both to resume the active clip. Selecting a "
            "different clip restarts it from t=0. Returns the now-playing clip; "
            "verify visually with capture_frame.";
        playTool.inputSchemaJson =
            "{\"type\":\"object\",\"properties\":{"
            "\"index\":{\"type\":\"integer\",\"description\":\"Clip index from list_animations.\"},"
            "\"name\":{\"type\":\"string\",\"description\":\"Clip name from list_animations.\"}}}";
        XrResult r2 = g_pfnRegisterMCPTool(g_xr->session, &playTool);

        XrMCPToolInfoDXR stopTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        stopTool.name = "stop_animation";
        stopTool.description =
            "Pause animation playback, freezing the model at its current pose. "
            "Resume with play_animation.";
        stopTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r3 = g_pfnRegisterMCPTool(g_xr->session, &stopTool);

        g_mcpAnimToolsRegistered = XR_SUCCEEDED(r1) || XR_SUCCEEDED(r2) || XR_SUCCEEDED(r3);
        LOG_INFO("XR_DXR_mcp_tools: animation tools registered (%d clip(s)) [%d %d %d]",
                 g_modelRenderer.animationCount(), r1, r2, r3);
    } else {
        g_pfnUnregisterMCPTool(g_xr->session, "list_animations");
        g_pfnUnregisterMCPTool(g_xr->session, "play_animation");
        g_pfnUnregisterMCPTool(g_xr->session, "stop_animation");
        g_mcpAnimToolsRegistered = false;
        LOG_INFO("XR_DXR_mcp_tools: animation tools unregistered (model has no clips)");
    }
}

// Forward-declared so RegisterMcpBaseTools can wire it as the mcpToolHandler
// hook (the definition, with the full tool dispatch, is below).
static std::string HandleMcpToolCall(XrSessionManager& xr,
                                     const std::string& toolName,
                                     const std::string& argsJson,
                                     bool& success);

// Declare the app identity + register the base agent tools. Call once, right
// after xrCreateSession. The appId MUST match `id` in
// windows/displayxr/avatar_handle_vk_win.displayxr.json (INV-10.1). The
// animation tools are NOT registered here — they appear only once a model with
// clips loads (UpdateMcpAnimationTools, driven from ApplyAutoFitForLoadedScene).
static void RegisterMcpBaseTools(XrSessionManager& xr) {
    if (!g_hasMcpToolsExt || !g_pfnSetMCPAppInfo || !g_pfnRegisterMCPTool ||
        xr.session == XR_NULL_HANDLE) {
        return;
    }
    XrMCPAppInfoDXR appInfo = {XR_TYPE_MCP_APP_INFO_DXR};
    strncpy(appInfo.appId, "avatar", sizeof(appInfo.appId) - 1);
    XrResult ar = g_pfnSetMCPAppInfo(xr.session, &appInfo);
    if (XR_FAILED(ar)) {
        LOG_INFO("XR_DXR_mcp_tools: appId not accepted (%d) — no agent surface", ar);
        return;
    }

    XrMCPToolInfoDXR loadTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    loadTool.name = "load_model";
    loadTool.description =
        "Load a 3D model file into the viewer, replacing the current model. "
        "Supported formats: glTF (.glb/.gltf), STL, OBJ, FBX, USD "
        "(.usdz/.usd/.usda/.usdc). The path must be absolute and readable by "
        "the viewer process. On success the camera re-frames the model "
        "automatically, and the animation tools (list_animations / "
        "play_animation / stop_animation) appear or disappear depending on "
        "whether the new model has animation clips.";
    loadTool.inputSchemaJson =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Absolute filesystem path of the model file to load.\"}},"
        "\"required\":[\"path\"]}";
    XrResult t1 = g_pfnRegisterMCPTool(xr.session, &loadTool);

    XrMCPToolInfoDXR statusTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    statusTool.name = "get_status";
    statusTool.description =
        "Read the viewer's live state: loaded model file and primitive count, "
        "animation clip count + active clip + playing flag, camera orbit "
        "(azimuth/elevation in degrees, world position, zoom factor), active "
        "rendering-mode index, and whether the XR session is running.";
    statusTool.inputSchemaJson = "{\"type\":\"object\"}";
    XrResult t2 = g_pfnRegisterMCPTool(xr.session, &statusTool);

    XrMCPToolInfoDXR orbitTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    orbitTool.name = "set_orbit";
    orbitTool.description =
        "Orbit the camera around the model. azimuth_deg rotates around the "
        "vertical axis (0 = the model's authored front, increasing turns the "
        "model to the right), elevation_deg tilts the view up/down (clamped to "
        "±85), zoom scales the model on screen (>1 = larger, clamped 0.1–10, "
        "default 1). All fields are optional; omitted ones keep their current "
        "value. Also resets the idle auto-orbit timer, like any user input. "
        "Verify the result visually with capture_frame.";
    orbitTool.inputSchemaJson =
        "{\"type\":\"object\",\"properties\":{"
        "\"azimuth_deg\":{\"type\":\"number\",\"description\":\"Orbit angle around the vertical axis, degrees.\"},"
        "\"elevation_deg\":{\"type\":\"number\",\"description\":\"Tilt above (+) / below (−) the horizon, degrees, clamped to ±85.\"},"
        "\"zoom\":{\"type\":\"number\",\"description\":\"View scale factor, 0.1–10; 1 = the auto-fit framing.\"}}}";
    XrResult t3 = g_pfnRegisterMCPTool(xr.session, &orbitTool);

    XrMCPToolInfoDXR frameTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    frameTool.name = "frame_model";
    frameTool.description =
        "Reset the camera to the loaded model's auto-fit framed pose (same as "
        "pressing Space): centers the model with comfortable headroom and "
        "restores zoom to 1. Requires a model to be loaded.";
    frameTool.inputSchemaJson = "{\"type\":\"object\"}";
    XrResult t4 = g_pfnRegisterMCPTool(xr.session, &frameTool);

    // Route tool-call dispatch through the shared PollEvents hook (common #18):
    // it fetches the args and submits our returned JSON, so we only map
    // (toolName, argsJson) -> resultJson. Set once here, alongside registration.
    xr.mcpToolHandler = [&xr](const std::string& toolName,
                              const std::string& argsJson, bool& success) {
        return HandleMcpToolCall(xr, toolName, argsJson, success);
    };

    g_mcpToolsReady = true;
    LOG_INFO("XR_DXR_mcp_tools: appId=avatar load_model=%d get_status=%d "
             "set_orbit=%d frame_model=%d", t1, t2, t3, t4);
}

// Dispatch one agent tool call. Invoked by the shared displayxr-common
// PollEvents through the mcpToolHandler hook (common #18): PollEvents has
// already fetched the JSON args and will submit whatever we return, so this is a
// pure (toolName, argsJson) -> resultJson map — it must NOT call
// xrGetMCPToolCallArgsDXR / xrSubmitMCPToolResultDXR. Runs on the render thread.
// EVERY call is answered — success=false + {"error":…} for bad args — because an
// unanswered call only fails to the agent after the runtime's ~5 s timeout.
static std::string HandleMcpToolCall(XrSessionManager& xr,
                                     const std::string& toolName,
                                     const std::string& argsJson,
                                     bool& success) {
    const char* a = argsJson.c_str();
    std::string result;
    success = true;
    char buf[1024];

    if (toolName == "load_model") {
        std::string path;
        if (!McpJsonGetString(a, "path", path) || path.empty()) {
            success = false;
            result = "{\"error\":\"missing required string argument 'path'\"}";
        } else if (!model_validate_file(path)) {
            success = false;
            result = "{\"error\":\"not a readable supported model file: " +
                     McpJsonEscape(path) + "\"}";
        } else {
            // Mirror TryAutoLoadBundledScene's locking: the scene mutex guards
            // the model swap, and ApplyAutoFitForLoadedScene_locked (which
            // (un)registers the animation tools) requires it held.
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            if (!g_modelRenderer.loadModel(path.c_str())) {
                success = false;
                result = "{\"error\":\"failed to load (corrupt or unsupported): " +
                         McpJsonEscape(path) + "\"}";
            } else {
                g_loadedFileName = model_basename(path);
                LOG_INFO("Model loaded via MCP: %s (%s)", g_loadedFileName.c_str(),
                         model_filesize_str(path).c_str());
                // Re-frames the camera and (un)registers the animation tools.
                ApplyAutoFitForLoadedScene_locked();
                snprintf(buf, sizeof(buf),
                         "{\"file\":\"%s\",\"primitives\":%u,\"animation_count\":%d}",
                         McpJsonEscape(g_loadedFileName).c_str(),
                         g_modelRenderer.primitiveCount(), g_modelRenderer.animationCount());
                result = buf;
            }
        }
    } else if (toolName == "get_status") {
        std::string clip; int ci = -1, cn = 0; float ct = 0, cd = 0; bool playing = false;
        const bool hasClip = g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing);
        float yaw, pitch, zoom, px, py, pz;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            yaw = g_inputState.yaw; pitch = g_inputState.pitch;
            zoom = g_inputState.viewParams.scaleFactor;
            px = g_inputState.cameraPosX; py = g_inputState.cameraPosY; pz = g_inputState.cameraPosZ;
        }
        const float azDeg = fmodf(yaw * 57.29578f, 360.0f);
        const float elDeg = pitch * 57.29578f;
        std::string clipJson = hasClip ? "\"" + McpJsonEscape(clip) + "\"" : "null";
        snprintf(buf, sizeof(buf),
                 "{\"file\":\"%s\",\"loaded\":%s,\"primitives\":%u,"
                 "\"animation_count\":%d,\"active_animation\":%d,"
                 "\"active_animation_name\":%s,\"animation_playing\":%s,"
                 "\"camera\":{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,"
                 "\"position\":[%.3f,%.3f,%.3f],\"zoom\":%.2f},"
                 "\"rendering_mode\":%u,\"session_running\":%s}",
                 McpJsonEscape(g_loadedFileName).c_str(),
                 g_modelRenderer.hasModel() ? "true" : "false",
                 g_modelRenderer.primitiveCount(),
                 g_modelRenderer.animationCount(),
                 hasClip ? ci : -1, clipJson.c_str(),
                 (hasClip && playing) ? "true" : "false",
                 azDeg, elDeg, px, py, pz, zoom,
                 xr.currentModeIndex,
                 xr.sessionRunning ? "true" : "false");
        result = buf;
    } else if (toolName == "set_orbit") {
        double az, el, zm;
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            if (McpJsonGetNumber(a, "azimuth_deg", az)) {
                g_inputState.yaw = (float)(az * 0.0174532925);
                any = true;
            }
            if (McpJsonGetNumber(a, "elevation_deg", el)) {
                if (el > 85.0) el = 85.0;
                if (el < -85.0) el = -85.0;
                g_inputState.pitch = (float)(el * 0.0174532925);
                any = true;
            }
            if (McpJsonGetNumber(a, "zoom", zm)) {
                if (zm < 0.1) zm = 0.1;
                if (zm > 10.0) zm = 10.0;
                g_inputState.viewParams.scaleFactor = (float)zm;
                any = true;
            }
            if (any) {
                // Agent input is input: reset the idle auto-orbit timer.
                using namespace std::chrono;
                g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
                    high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
                g_inputState.animationActive = false;
            }
        }
        if (!any) {
            success = false;
            result = "{\"error\":\"provide at least one of azimuth_deg, elevation_deg, zoom\"}";
        } else {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            snprintf(buf, sizeof(buf),
                     "{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,\"zoom\":%.2f}",
                     g_inputState.yaw * 57.29578f, g_inputState.pitch * 57.29578f,
                     g_inputState.viewParams.scaleFactor);
            result = buf;
        }
    } else if (toolName == "frame_model") {
        if (!g_modelRenderer.hasModel()) {
            success = false;
            result = "{\"error\":\"no model loaded — call load_model first\"}";
        } else {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.resetViewRequested = true;  // applied by the render loop next frame
            result = "{\"framed\":true}";
        }
    } else if (toolName == "list_animations") {
        const int n = g_modelRenderer.animationCount();
        std::string clips = "[";
        for (int i = 0; i < n; i++) {
            std::string nm; float dur = 0;
            g_modelRenderer.getAnimationInfo(i, nm, dur);
            snprintf(buf, sizeof(buf), "%s{\"index\":%d,\"name\":\"%s\",\"duration_s\":%.2f}",
                     i ? "," : "", i, McpJsonEscape(nm).c_str(), dur);
            clips += buf;
        }
        clips += "]";
        snprintf(buf, sizeof(buf), ",\"active_index\":%d,\"playing\":%s}",
                 g_modelRenderer.activeAnimation(),
                 (g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused()) ? "true" : "false");
        result = "{\"animations\":" + clips + buf;
    } else if (toolName == "play_animation") {
        const int n = g_modelRenderer.animationCount();
        int target = -1;
        double idx; std::string nm;
        if (McpJsonGetNumber(a, "index", idx)) {
            target = (int)idx;
            if (target < 0 || target >= n) {
                success = false;
                snprintf(buf, sizeof(buf), "{\"error\":\"index out of range (0..%d)\"}", n - 1);
                result = buf;
            }
        } else if (McpJsonGetString(a, "name", nm)) {
            for (int i = 0; i < n && target < 0; i++) {
                std::string c; float d;
                g_modelRenderer.getAnimationInfo(i, c, d);
                if (c == nm) target = i;
            }
            if (target < 0) {
                success = false;
                result = "{\"error\":\"no clip named '" + McpJsonEscape(nm) +
                         "' — see list_animations\"}";
            }
        } else {
            target = g_modelRenderer.activeAnimation();  // resume the active clip
            if (target < 0) target = 0;
        }
        if (success) {
            if (n == 0) {
                success = false;
                result = "{\"error\":\"the loaded model has no animation clips\"}";
            } else {
                if (target != g_modelRenderer.activeAnimation())
                    g_modelRenderer.setActiveAnimation(target);
                g_modelRenderer.setPaused(false);
                std::string c; float d = 0;
                g_modelRenderer.getAnimationInfo(target, c, d);
                snprintf(buf, sizeof(buf),
                         "{\"playing\":\"%s\",\"index\":%d,\"duration_s\":%.2f}",
                         McpJsonEscape(c).c_str(), target, d);
                result = buf;
            }
        }
    } else if (toolName == "stop_animation") {
        g_modelRenderer.setPaused(true);
        std::string c; float d = 0;
        const int active = g_modelRenderer.activeAnimation();
        if (active >= 0) g_modelRenderer.getAnimationInfo(active, c, d);
        snprintf(buf, sizeof(buf), "{\"playing\":false,\"paused_clip\":%s%s%s}",
                 active >= 0 ? "\"" : "", active >= 0 ? McpJsonEscape(c).c_str() : "null",
                 active >= 0 ? "\"" : "");
        result = buf;
    } else {
        success = false;
        result = "{\"error\":\"unhandled tool\"}";
    }

    return result;
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

// Speech-bubble client-window rect, published by the render thread and unioned
// into the window region by UpdateClickRegion so the bubble (which sits above
// the tiger, outside the silhouette) isn't clipped away by the shaped window.
static std::mutex g_bubbleMtx;
static RECT       g_bubbleRect = {0, 0, 0, 0};
static bool       g_bubbleVisible = false;

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
        // Skip the top 25%: the avatar is confined to the bottom 75%, and the
        // top-25% silhouette pixels are stale/untouched (the bubble rect is added
        // to the region separately below).
        const int yStart = (ch * 1) / 4;
        for (int y = yStart; y < ch; ++y) {
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

    // Add the speech-bubble rect so it stays visible (and clickable) even though
    // it lies outside the tiger silhouette that otherwise shapes the window.
    {
        std::lock_guard<std::mutex> bl(g_bubbleMtx);
        if (g_bubbleVisible && g_bubbleRect.right > g_bubbleRect.left &&
            g_bubbleRect.bottom > g_bubbleRect.top) {
            rects.push_back(g_bubbleRect);
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
    case WM_LBUTTONDOWN:
        // The avatar has no on-screen buttons — a left press just starts a
        // scene-rotate drag (UpdateInputState above set leftButton/dragging).
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
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
        // V key = cycle display rendering mode (2D/3D/...). Standalone only —
        // under the workspace the controller owns the V toggle and routes mode
        // changes over IPC, so this never fires there. Main loop reads the flag
        // and computes the target from the runtime's current mode.
        if (wParam == 'V') {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cycleRenderingModeRequested = true;
            return 0;
        }
        // I key = capture multi-view atlas
        if (wParam == 'I' || wParam == 'i') {
            g_captureAtlasRequested.store(true);
        }
        // G key = toggle the alpha edge-softening post-pass (Gaussian silhouette
        // blur). 8× MSAA is always on; this extra per-eye pass is opt-in so the
        // softening / its cost can be A/B'd live on the display.
        if (wParam == 'G') {
            bool now = !g_modelRenderer.edgeSoftenEnabled();
            g_modelRenderer.setEdgeSoftenEnabled(now);
            LOG_INFO("Edge-soften post-pass: %s (G)", now ? "ON" : "OFF");
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
    // client rect on the monitor hosting the 3D panel so the borderless window
    // isn't stranded at (0,0) with no title bar to grab.
    //
    // INV-1.3 (runtime#715): (g_displayDesktopLeft, g_displayDesktopTop) is the
    // panel's top-left in virtual-desktop pixels (XrDisplayDesktopPositionDXR,
    // display_info v16) — on a multi-monitor box the window must open ON the
    // panel, since the window-relative weave is only correct there. (0,0) =
    // primary/unknown, which MONITOR_DEFAULTTOPRIMARY resolves to the old
    // primary-monitor behavior.
    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    {
        HMONITOR hMon = MonitorFromPoint(POINT{g_displayDesktopLeft, g_displayDesktopTop},
                                         MONITOR_DEFAULTTOPRIMARY);
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

// Render the avatar silhouette into the scratch image, read the alpha back,
// and publish the coverage bitmap consumed by WM_NCHITTEST. Cheap (downscaled to
// ~1/3 the window). Runs on the render thread right after the main eye render so
// it reuses the renderer's internal targets (no resize thrash — imgW/imgH must
// match the swapchain dims the main eye render passed, else ensureTargets churns
// every frame as the two callers alternate sizes).
//
// The hit mask is the UNION of the FIRST and LAST active views: the weave shows
// every view at once with horizontal disparity that grows with window size — a
// single-view mask visibly clips the other view's tiger on a large window. The
// two outermost views bound the spread (middle views of a quad layout fall
// between them for the convex-ish tiger).
static void UpdateSilhouette(VkDevice dev, VkPhysicalDevice phys, VkQueue queue, VkCommandPool pool,
                            uint32_t imgW, uint32_t imgH, uint32_t winW, uint32_t winH,
                            const float (*viewMats)[16], const float (*projMats)[16],
                            const float* clipFars, uint32_t numViews) {
    if (winW == 0 || winH == 0 || numViews == 0) return;
    uint32_t w = winW / 3; if (w < 64) w = 64; if (w > 640) w = 640;
    uint32_t h = winH / 3; if (h < 64) h = 64; if (h > 360) h = 360;
    if (!EnsureSilhouetteTargets(dev, phys, w, h)) return;

    const uint32_t silIdx[2] = {0, numViews - 1};
    const uint32_t silPasses = (numViews > 1) ? 2u : 1u;
    std::vector<uint8_t> unionAlpha((size_t)w * h, 0);

    for (uint32_t p = 0; p < silPasses; ++p) {
        const uint32_t v = silIdx[p];
        {
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            if (!g_modelRenderer.hasModel()) return;
            // Render the avatar into the bottom 75% of the silhouette image too, so
            // the hit mask matches the confined on-screen avatar (the zone-framed
            // views map 1:1 onto this sub-viewport — same NDC mapping). The top 25%
            // is left as-is (covered by the full-top-25% bubble rect in the click
            // region).
            const uint32_t silAvH = (h * 3u) / 4u;
            const uint32_t silAvY = h - silAvH;
            g_modelRenderer.renderEye(g_silImage.image, VK_FORMAT_R8G8B8A8_UNORM,
                imgW, imgH,
                0, silAvY, w, silAvH, viewMats[v], projMats[v], /*transparentBg=*/true,
                clipFars[v]);
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
        for (uint32_t i = 0; i < w * h; ++i) {
            const uint8_t a = px[i * 4 + 3];
            if (a > unionAlpha[i]) unionAlpha[i] = a;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_silCoverage.mtx);
        g_silCoverage.bits.resize((size_t)w * h);
        for (uint32_t i = 0; i < w * h; ++i) g_silCoverage.bits[i] = (unionAlpha[i] > 40) ? 1 : 0;
        g_silCoverage.covW = (int)w; g_silCoverage.covH = (int)h;
        g_silCoverage.winW = (int)winW; g_silCoverage.winH = (int)winH;
        g_silCoverage.ready = true;
    }

    // Debug: with DXR_DUMP_SILHOUETTE set, dump the silhouette alpha (~once/sec)
    // to %TEMP%\avatar_silhouette.png so the hit mask can be eyeballed (white =
    // avatar, black = pass-through). Off by default.
    static const bool s_dumpSil = (GetEnvironmentVariableA("DXR_DUMP_SILHOUETTE", nullptr, 0) > 0);
    static int s_dbgCounter = 0;
    if (s_dumpSil && (s_dbgCounter++ % 60) == 0) {
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        std::string path = std::string(tmp) + "avatar_silhouette.png";
        stbi_write_png(path.c_str(), (int)w, (int)h, 1, unionAlpha.data(), (int)w);
    }
}

// ── Display-zones path (ADR-027 / XR_DXR_display_zones, P6 migration) ───────
// The tiger renders through ONE 3D zone (zoneId 1, rect = bottom 75% of the
// client window) with the runtime rig (XR_DXR_view_rig) chained on the zone
// locate — the app consumes render-ready XrView pose/fov framed to the rect
// and the app-side Kooima is gone. The speech bubble stays a plain Local2D
// layer in the top 25% (in a zones frame Local2D is pure 2D content; the
// wish auto-derives from the zone rect, no mask object).

// Column-major float[16] helpers for consuming XrView pose/fov, ported from
// the runtime's cube_zones_vk_macos reference consumer.
static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_translation(float* m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            r[c * 4 + rw] = a[0 * 4 + rw] * b[c * 4 + 0] + a[1 * 4 + rw] * b[c * 4 + 1] +
                            a[2 * 4 + rw] * b[c * 4 + 2] + a[3 * 4 + rw] * b[c * 4 + 3];
    memcpy(out, r, sizeof(r));
}

// Asymmetric frustum from the runtime-returned fov. GL convention ([-1,1]
// clip-z) — remap with convert_projection_gl_to_zero_to_one for Vulkan.
static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

// view = inverse(pose): R^T * translate(-p). Plain +Y-up world — the renderer
// runs in the plain view convention on the zones path (negative-height
// viewport handles the Vulkan Y-flip, see ModelRenderer::setPlainViewConvention).
static void mat4_view_from_xr_pose(float* viewMat, const XrPosef& pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);

    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];

    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

// Rotate vec3 by quaternion: v' = q * v * q^-1
static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye distance for the ZDP-anchored clip: z of (rigPose^-1 *
// eyeWorld). Same quantity the app-side Kooima reported as eye_display.z.
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// Zone state (render-thread-owned). The zone swapchain is pre-sized once at
// activation for the fullscreen worst case so live resize never recreates it
// (same philosophy as the main swapchain's largest-atlas pre-size); per-frame
// tile dims come from xrGetDisplayZoneRecommendedViewSizeDXR clamped to the
// capacity.
static SwapchainInfo        g_zoneSwapchain;
static std::vector<VkImage> g_zoneSwapImages;
static bool g_zonesActive = false;
static bool g_zonesTried  = false;

// DXR_ZONES_FADE_PX: tiger-zone edge feather width in px (content alpha, ADR-027
// rule 4). Default 16; 0 disables. DXR_ZONES_VALIDATE=1 chains the frame-end
// validate bit (locate/submit pairing diagnostics) — bring-up only.
static float EnvFadePx() {
    char buf[16] = {};
    if (GetEnvironmentVariableA("DXR_ZONES_FADE_PX", buf, sizeof(buf)) > 0)
        return (float)atof(buf);
    return 16.0f;
}
static bool EnvZonesValidate() {
    char buf[8] = {};
    return GetEnvironmentVariableA("DXR_ZONES_VALIDATE", buf, sizeof(buf)) > 0 &&
           buf[0] != '0';
}

// One-time zones activation: caps query + zone swapchain creation. Returns on
// the first call with valid window dims; sets g_zonesActive on success. On an
// old runtime (extension absent / unsupported) the app falls back to the
// raw-views path — degraded flat rendering, one-shot WARN.
static void TryActivateZones(XrSessionManager* xr) {
    if (g_zonesTried) return;
    g_zonesTried = true;

    if (!g_hasDisplayZonesExt || !g_hasViewRigExt ||
        !g_pfnGetDisplayZoneCaps || !g_pfnGetDisplayZoneViewSize) {
        LOG_WARN("Display zones unavailable (display_zones=%d view_rig=%d) — "
                 "falling back to raw views (flat rendering)",
                 (int)g_hasDisplayZonesExt, (int)g_hasViewRigExt);
        return;
    }

    XrDisplayZoneCapabilitiesDXR caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
    XrResult cr = g_pfnGetDisplayZoneCaps(xr->session, &caps);
    if (XR_FAILED(cr) || !caps.supported || caps.maxZones3D < 1) {
        LOG_WARN("Display zones not supported by this session (result=0x%x "
                 "supported=%d maxZones3D=%u) — falling back to raw views",
                 (unsigned)cr, (int)caps.supported, caps.maxZones3D);
        return;
    }

    // Fullscreen worst case: widest mode tiling × native display resolution
    // (the zone rect can grow to the full window ≤ the panel; recommended view
    // size is rect-extent-sized). Height capacity covers the bottom-75% zone
    // of a fullscreen window.
    uint32_t maxCols = 2, maxRows = 2;
    for (uint32_t m = 0; m < xr->renderingModeCount; m++) {
        if (xr->renderingModeTileColumns[m] > maxCols) maxCols = xr->renderingModeTileColumns[m];
        if (xr->renderingModeTileRows[m]    > maxRows) maxRows = xr->renderingModeTileRows[m];
    }
    uint32_t dispW = xr->displayPixelWidth  ? xr->displayPixelWidth  : xr->swapchain.width;
    uint32_t dispH = xr->displayPixelHeight ? xr->displayPixelHeight : xr->swapchain.height;
    uint32_t capW = maxCols * dispW;
    uint32_t capH = maxRows * ((dispH * 3u) / 4u);

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = xr->swapchain.format;   // reuse the negotiated main format
    ci.sampleCount = 1;
    ci.width = capW;
    ci.height = capH;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr->session, &ci, &g_zoneSwapchain.swapchain))) {
        LOG_WARN("Zone swapchain creation failed (%ux%u) — falling back to raw views",
                 capW, capH);
        g_zoneSwapchain.swapchain = XR_NULL_HANDLE;
        return;
    }
    g_zoneSwapchain.format = xr->swapchain.format;
    g_zoneSwapchain.width = capW;
    g_zoneSwapchain.height = capH;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_zoneSwapchain.swapchain, 0, &n, nullptr);
    g_zoneSwapchain.imageCount = n;
    std::vector<XrSwapchainImageVulkanKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g_zoneSwapchain.swapchain, n, &n,
        (XrSwapchainImageBaseHeader*)imgs.data());
    g_zoneSwapImages.resize(n);
    for (uint32_t i = 0; i < n; i++) g_zoneSwapImages[i] = imgs[i].image;

    g_zonesActive = true;
    LOG_WARN("Display zones ACTIVE: maxZones3D=%u, zone swapchain %ux%u (%u images)",
             caps.maxZones3D, capW, capH, n);
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkDevice vkDevice,
    VkQueue graphicsQueue,
    uint32_t queueFamilyIndex,
    VkInstance vkInstance,
    VkPhysicalDevice physDevice,
    std::vector<VkImage>* swapchainVkImages)
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
        bool cycleClip = false;
        bool playPause = false;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
        }
        // The avatar stays UPRIGHT: mouse-drag must not pitch it (the character
        // faces the viewer via the yaw billboard; tilting it forward/back looks
        // wrong). Pin renderPitch to 0 so drag-pitch is ignored everywhere it's
        // consumed (display pose, mono matrices, pick). The shared input handler
        // still accumulates inputSnapshot.pitch on drag, but nothing renders it.
        float renderPitch = 0.0f;
        // Manual LMB drag must be fully inert: the billboard owns the heading, so
        // drag can't rotate the tiger — but the drag-accumulated yaw/pitch would
        // still rotate the WASD/EQ MOVEMENT frame inside UpdateCameraMovement
        // (it builds its basis from state.yaw/pitch). Pin both to 0 before the
        // movement step so movement stays display-aligned; the writeback below
        // persists the zeros into g_inputState, discarding the drag deltas.
        inputSnapshot.yaw = 0.0f;
        inputSnapshot.pitch = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            resetRequested = g_inputState.resetViewRequested;
            animateToggle = g_inputState.animateToggleRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.teleportRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            // ModeSwitch consumes the V/0-8 flags off inputSnapshot (captured
            // above); clear them on the shared state so they fire exactly once.
            g_inputState.cycleRenderingModeRequested = false;
            g_inputState.absoluteRenderingModeRequested = -1;
            g_inputState.eyeTrackingModeToggleRequested = false;
            if (g_inputState.transparentBgToggleRequested) {
                g_inputState.transparentBgToggleRequested = false;
                bool now = !g_transparentBg.load();
                g_transparentBg.store(now);
                LOG_INFO("Transparent background: %s (Ctrl+T)", now ? "ON" : "OFF");
            }
            g_inputState.animateToggleRequested = false;
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

        // Rendering mode requests (V/mode-button=cycle, 0-8=absolute) through the
        // shared ModeSwitch sequencer: eases viewParams.ipdFactor around the switch
        // and fires xrRequestDisplayRenderingModeDXR on the right frame. Ramped ipd
        // lands on inputSnapshot.viewParams.ipdFactor (what the render path reads).
        // Runtime owns current mode via xr->currentModeIndex.
        XrSessionUpdateModeSwitch(*xr, inputSnapshot, perfStats.deltaTime);

        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeDXR newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                    ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
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

        // Bind the virtual-display rig to a moving/skinned subject. We center the
        // avatar VERTICALLY on the smoothed skeleton centroid (so the animation's
        // own bob doesn't slide it off-screen), and horizontally on the centroid
        // PLUS the user's A/D pan — cameraPosX accumulates the A/D strafe from
        // UpdateCameraMovement, based at g_fitCenter[0], so (cameraPosX - fitCentre)
        // is the net horizontal pan since the last reset. Z stays the user's W/S
        // depth dolly. So: A/D = slide left/right, W/S = depth, all on top of the
        // drift-removing anchor. Applied to inputSnapshot only; g_inputState keeps
        // the accumulated pose (and the pan resets to 0 on Space, which restores
        // cameraPosX = g_fitCenter[0]).
        {
            float anchor[3];
            if (g_fitValid.load() && g_modelRenderer.getAnimatedAnchor(anchor)) {
                const float kPanSign = -1.0f;  // D = avatar right; flip if reversed
                inputSnapshot.cameraPosX = anchor[0] + kPanSign * (inputSnapshot.cameraPosX - g_fitCenter[0]);
                inputSnapshot.cameraPosY = anchor[1];
                // cameraPosZ intentionally left as the user's W/S-driven value.
            }
        }

        // Shared displayxr-common pump: session bookkeeping + XR_DXR_mcp_tools
        // dispatch via the app-supplied mcpToolHandler hook (common #18, #30).
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                // Sized to runtime's max possible view count (sim_display Quad mode = 4).
                // Active mode's view count drives how many slots are actually filled and submitted.
                XrCompositionLayerProjectionView projectionViews[8] = {};
                bool rendered = false;

                // Display zones: the SAME XrDisplayZoneDXR instance chains on the
                // zone-scoped locate (with the rig) AND on the submitted projection
                // layer (next reset to NULL) — one variable, both chain points.
                XrDisplayZoneDXR tigerZone = {XR_TYPE_DISPLAY_ZONE_DXR};
                XrDisplayRigDXR  tigerRig  = {XR_TYPE_DISPLAY_RIG_DXR};
                bool zonesFrame = false;

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, renderPitch,
                        inputSnapshot.viewParams)) {

                        // One-time zones bring-up (caps query + zone swapchain).
                        TryActivateZones(xr);

                        // Raw locate: with nothing chained, XrView.pose carries the
                        // raw display-space eyes (external-window transport). Feeds
                        // the billboard head centroid and the no-zones fallback;
                        // the zones path locates AGAIN below, zone-scoped.
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
                        // per-view buffers (zoneViews / viewMat / projectionViews)
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

                        // ── Zone-scoped locate (runtime rig replaces the app-side
                        //    Kooima — ADR-027 P6). ONE zone: the tiger, bottom 75%
                        //    of the client window. The runtime owns the window/canvas
                        //    geometry (framing follows the window server-side), so the
                        //    old ClientToScreen/MonitorFromWindow eye-offset math is
                        //    gone with the Kooima. ──────────────────────────────────
                        XrView zoneViews[8];
                        uint32_t zoneViewCount = 0;
                        uint32_t tileW = 0, tileH = 0;
                        zonesFrame = g_zonesActive && windowW > 0 && windowH > 0;
                        if (zonesFrame) {
                            // ── Face-the-viewer billboard (yaw-only) ───────────
                            // Head centroid from the raw display-space eyes (the
                            // un-chained locate above), rebased to the ZONE canvas
                            // centre using the runtime-reported canvas rect from
                            // LAST frame's zone locate (raw eyes are panel-relative,
                            // not canvas-rebased — XR_DXR_view_rig). One frame of
                            // staleness is invisible under the tau smoothing. First
                            // frame (no canvas yet): hold facing-forward, exactly
                            // like the eye-tracking warmup hold.
                            static XrRect2Di s_zoneCanvasPx = {};
                            static bool s_zoneCanvasValid = false;
                            {
                                static constexpr float FACE_YAW_SIGN = -1.0f;  // viewer-confirmed
                                float cx, cy, cz;
                                if (eyeCount <= 1 || viewCount < 2) {
                                    cx = rawViews[0].pose.position.x;
                                    cy = rawViews[0].pose.position.y;
                                    cz = rawViews[0].pose.position.z;
                                } else {
                                    cx = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                                    cy = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                                    cz = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;
                                }
                                (void)cy;
                                float hx = cx, hz = cz;
                                if (s_zoneCanvasValid && xr->displayPixelWidth > 0 &&
                                    xr->displayWidthM > 0.0f) {
                                    // Canvas-centre offset from the display centre, in
                                    // metres, +X right (panel px are y-down; yaw only
                                    // needs X).
                                    const float pxSizeX = xr->displayWidthM / (float)xr->displayPixelWidth;
                                    const float canvasCxPx = (float)s_zoneCanvasPx.offset.x +
                                                             (float)s_zoneCanvasPx.extent.width * 0.5f;
                                    hx = cx - (canvasCxPx - (float)xr->displayPixelWidth * 0.5f) * pxSizeX;
                                }
                                const float hzAbs = fabsf(hz) > 1e-3f ? fabsf(hz) : 1e-3f;
                                float targetYaw = FACE_YAW_SIGN * atan2f(hx, hzAbs);
                                // Exponential shortest-angle smoothing, TIME-BASED so
                                // it's frame-rate independent (a per-frame factor made
                                // the billboard stutter under uneven frame timing).
                                static float s_faceYaw = 0.0f;
                                const float tau = 0.04f;  // seconds (settle ≈ 3·tau)
                                float a = 1.0f - expf(-perfStats.deltaTime / tau);
                                if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
                                // Only chase the head while eye tracking is LOCKED —
                                // warmup positions would jitter the heading.
                                if (xr->isEyeTracking) {
                                    float dy = targetYaw - s_faceYaw;
                                    while (dy >  3.14159265f) dy -= 6.28318531f;
                                    while (dy < -3.14159265f) dy += 6.28318531f;
                                    s_faceYaw += dy * a;
                                }
                                inputSnapshot.yaw = s_faceYaw;
                            }

                            // Rig: the ex-app-side Kooima tunables, 1:1. Pose is the
                            // PLAIN world frame (no Y negation — that existed only to
                            // match the renderer's view-stage mirror, which the zones
                            // path retires via setPlainViewConvention).
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                                renderPitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            tigerRig.next = nullptr;
                            tigerRig.pose.orientation = {q.x, q.y, q.z, q.w};
                            tigerRig.pose.position = {inputSnapshot.cameraPosX,
                                                      inputSnapshot.cameraPosY,
                                                      inputSnapshot.cameraPosZ};
                            tigerRig.virtualDisplayHeight =
                                inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;
                            tigerRig.ipdFactor         = inputSnapshot.viewParams.ipdFactor;
                            tigerRig.parallaxFactor    = inputSnapshot.viewParams.parallaxFactor;
                            tigerRig.perspectiveFactor = inputSnapshot.viewParams.perspectiveFactor;

                            // Zone rect = bottom 75% of the client window (client px,
                            // y-down), recomputed every frame so live resize tracks.
                            tigerZone.zoneId = 1;
                            tigerZone.rect.offset = {0, (int32_t)(windowH / 4u)};
                            tigerZone.rect.extent = {(int32_t)windowW,
                                                     (int32_t)(windowH - windowH / 4u)};

                            // Per-zone recommended view size; re-query when the rect
                            // dims or the rendering mode change (the app-side
                            // equivalent of XrEventDataDisplayZoneMetricsChangedDXR,
                            // which displayxr-common's PollEvents drops unseen).
                            // Clamped to the pre-sized swapchain capacity per tile.
                            static XrExtent2Di s_zoneViewSize = {0, 0};
                            static int32_t  s_lastZoneW = -1, s_lastZoneH = -1;
                            static uint32_t s_lastZoneMode = 0xFFFFFFFFu;
                            if (tigerZone.rect.extent.width  != s_lastZoneW ||
                                tigerZone.rect.extent.height != s_lastZoneH ||
                                xr->currentModeIndex != s_lastZoneMode) {
                                if (XR_FAILED(g_pfnGetDisplayZoneViewSize(
                                        xr->session, &tigerZone.rect, &s_zoneViewSize)) ||
                                    s_zoneViewSize.width <= 0 || s_zoneViewSize.height <= 0) {
                                    s_zoneViewSize = tigerZone.rect.extent;
                                }
                                s_lastZoneW = tigerZone.rect.extent.width;
                                s_lastZoneH = tigerZone.rect.extent.height;
                                s_lastZoneMode = xr->currentModeIndex;
                            }
                            tileW = (uint32_t)s_zoneViewSize.width;
                            tileH = (uint32_t)s_zoneViewSize.height;
                            if (tileW * cols > g_zoneSwapchain.width)  tileW = g_zoneSwapchain.width / cols;
                            if (tileH * rows > g_zoneSwapchain.height) tileH = g_zoneSwapchain.height / rows;
                            if (tileW == 0) tileW = 1;
                            if (tileH == 0) tileH = 1;

                            // Zone-scoped locate: runtime Kooima framed to the rect.
                            // views[i].pose/fov come back render-ready; the raw
                            // readback reports the canvas the runtime resolved (the
                            // zone rect on the panel) for the billboard rebase.
                            tigerZone.next = &tigerRig;
                            XrViewLocateInfo zoneLocate = {XR_TYPE_VIEW_LOCATE_INFO};
                            zoneLocate.next = &tigerZone;
                            zoneLocate.viewConfigurationType = xr->viewConfigType;
                            zoneLocate.displayTime = frameState.predictedDisplayTime;
                            zoneLocate.space = xr->localSpace;
                            XrViewDisplayRawDXR zoneRaw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
                            XrViewState zoneViewState = {XR_TYPE_VIEW_STATE};
                            zoneViewState.next = &zoneRaw;
                            for (uint32_t i = 0; i < 8; i++) zoneViews[i] = {XR_TYPE_VIEW};
                            XrResult zlr = xrLocateViews(xr->session, &zoneLocate, &zoneViewState,
                                                         8, &zoneViewCount, zoneViews);
                            if (XR_SUCCEEDED(zlr) && zoneViewCount > 0) {
                                if (zoneRaw.canvasRectPx.extent.width > 0) {
                                    s_zoneCanvasPx = zoneRaw.canvasRectPx;
                                    s_zoneCanvasValid = true;
                                }
                            } else {
                                static bool s_zoneLocateWarned = false;
                                if (!s_zoneLocateWarned) {
                                    s_zoneLocateWarned = true;
                                    LOG_WARN("Zone-scoped xrLocateViews failed (0x%x) — raw-views fallback",
                                             (unsigned)zlr);
                                }
                                zonesFrame = false;
                            }
                        }

                        // Double-click focus: center-eye ray through mouse, pick splat,
                        // smoothly re-pose the virtual display to face back along the ray.
                        // The ray comes from the zone-scoped views (already in the plain
                        // world frame, same frame as pickSurface's geometry); the mouse
                        // maps over the ZONE rect — clicks in the bubble band don't pick.
                        if (inputSnapshot.teleportRequested && zonesFrame &&
                            inputSnapshot.teleportMouseY >= (float)tigerZone.rect.offset.y) {
                            float ndcX = 2.0f * (inputSnapshot.teleportMouseX - (float)tigerZone.rect.offset.x) /
                                         (float)tigerZone.rect.extent.width - 1.0f;
                            float ndcY = -(2.0f * (inputSnapshot.teleportMouseY - (float)tigerZone.rect.offset.y) /
                                          (float)tigerZone.rect.extent.height - 1.0f);

                            // Center pose = view centroid (views are spread about the rig
                            // centre), view-0 orientation/fov — close enough for a pick ray.
                            XrPosef centerPose = zoneViews[0].pose;
                            if (zoneViewCount >= 2) {
                                centerPose.position.x = (zoneViews[0].pose.position.x + zoneViews[1].pose.position.x) * 0.5f;
                                centerPose.position.y = (zoneViews[0].pose.position.y + zoneViews[1].pose.position.y) * 0.5f;
                                centerPose.position.z = (zoneViews[0].pose.position.z + zoneViews[1].pose.position.z) * 0.5f;
                            }
                            float centerViewMat[16], centerProjMat[16];
                            mat4_view_from_xr_pose(centerViewMat, centerPose);
                            const float pick_vH = tigerRig.virtualDisplayHeight;
                            const float pick_ez = RigLocalEyeZ(tigerRig.pose, centerPose.position);
                            const float pick_near = (pick_ez - pick_vH > 0.001f) ? (pick_ez - pick_vH) : 0.001f;
                            const float pick_far  = g_transparentBg.load() ? pick_ez : pick_ez + 1000.0f * pick_vH;
                            mat4_from_xr_fov(centerProjMat, zoneViews[0].fov, pick_near, pick_far);

                            XrVector3f rayOriginV, rayDirV;
                            display3d_unproject_ndc_to_ray(ndcX, ndcY,
                                centerViewMat, centerProjMat,
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

                        // Mono center eye — FALLBACK path only (the zone-scoped locate
                        // returns the center view per active mode, no app averaging).
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode && !zonesFrame) {
                            monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                            monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                            monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;

                            {
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
                            if (zonesFrame) {
                                // Render-ready zone views: view = inverse(pose),
                                // projection from the runtime fov. Near/far are
                                // app-side (the rig is clip-independent), anchored
                                // exactly like the old Kooima: ZDP-relative offsets
                                // in virtual-display-height units from the per-eye
                                // eye->display-plane distance. Transparent mode
                                // clips at the ZDP (far = ez); opaque keeps a large
                                // recede band.
                                const XrView& zv = zoneViews[(uint32_t)eye < zoneViewCount ? eye : zoneViewCount - 1];
                                const float vH = tigerRig.virtualDisplayHeight;
                                const float ez = RigLocalEyeZ(tigerRig.pose, zv.pose.position);
                                const float nearZ = (ez - vH > 0.001f) ? (ez - vH) : 0.001f;
                                const float farZ  = g_transparentBg.load() ? ez : ez + 1000.0f * vH;
                                mat4_view_from_xr_pose(viewMat[eye], zv.pose);
                                mat4_from_xr_fov(projMat[eye], zv.fov, nearZ, farZ);
                                // GL ([-1,1] clip-z) -> Vulkan [0,1].
                                convert_projection_gl_to_zero_to_one(projMat[eye]);
                                // ez = eye->display-plane forward distance, same world
                                // units as the shader's p_view.z (ex eye_display.z).
                                if (foregroundClip) {
                                    clipFar[eye] = (ez > 0.2f) ? ez : 0.0f;  // never cull at/behind near
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

                        // Zones path: render into the dedicated zone swapchain at
                        // FULL tile size (the zone rect IS the bottom 75% — no
                        // sub-viewport confinement). Fallback: legacy main-swapchain
                        // atlas with the bottom-75% sub-viewport.
                        static const float s_fadePx = EnvFadePx();
                        uint32_t imageIndex = 0;
                        const bool imageAcquired = zonesFrame
                            ? AcquireWindowSpaceImage(g_zoneSwapchain, imageIndex)
                            : AcquireSwapchainImage(*xr, imageIndex);
                        if (imageAcquired) {
                            const VkImage targetImage = zonesFrame
                                ? g_zoneSwapImages[imageIndex] : (*swapchainVkImages)[imageIndex];
                            const VkFormat colorFormat = (VkFormat)(zonesFrame
                                ? g_zoneSwapchain.format : xr->swapchain.format);
                            const uint32_t targetW = zonesFrame ? g_zoneSwapchain.width  : xr->swapchain.width;
                            const uint32_t targetH = zonesFrame ? g_zoneSwapchain.height : xr->swapchain.height;

                            bool hasGsScene;
                            {
                                std::lock_guard<std::mutex> lock(g_sceneMutex);
                                hasGsScene = g_modelRenderer.hasModel();
                            }

                            // Zone views are render-ready (un-mirrored) — the renderer
                            // runs the plain +Y-up convention (negative-height viewport).
                            // The fallback's DirectXMath matrices keep the legacy mirror.
                            // Per-frame so a transient zone-locate failure stays correct.
                            g_modelRenderer.setPlainViewConvention(zonesFrame);

                            if (hasGsScene) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    // Row-major eye placement in the atlas; for 2×1 SBS
                                    // this is (0, renderW) at row 0; for mono (cols=1)
                                    // it collapses to (0, 0).
                                    uint32_t col = (uint32_t)eye % cols;
                                    uint32_t row = (uint32_t)eye / cols;
                                    if (zonesFrame) {
                                        // Full tile + content-alpha edge feather
                                        // (ADR-027 rule 4 — the wish mask can't
                                        // carry per-zone fades).
                                        g_modelRenderer.renderEye(
                                            targetImage, colorFormat,
                                            targetW, targetH,
                                            col * tileW, row * tileH, tileW, tileH,
                                            viewMat[eye], projMat[eye],
                                            g_transparentBg.load(), clipFar[eye],
                                            s_fadePx);
                                    } else {
                                        uint32_t vpX = col * renderW;
                                        uint32_t vpY = row * renderH;
                                        // Confine the avatar to the BOTTOM 75% of the tile (→
                                        // bottom 75% of the window). The top 25% of the tile is
                                        // left untouched — the full-top-25% Local2D bubble's
                                        // implicit mask (M=0) replaces the weave there with the
                                        // flat bubble, so those pixels are never shown.
                                        const uint32_t avH = (renderH * 3u) / 4u;
                                        const uint32_t avY = vpY + (renderH - avH);
                                        g_modelRenderer.renderEye(
                                            targetImage, colorFormat,
                                            targetW, targetH,
                                            vpX, avY, renderW, avH,
                                            viewMat[eye], projMat[eye],
                                            g_transparentBg.load(), clipFar[eye]);
                                    }
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, renderCmdPool,
                                    targetImage, targetW, targetH);
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasDXR
                            // (XR_DXR_atlas_capture, W6 of #396). The runtime owns
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
                                    XrAtlasCaptureInfoDXR info = {XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR;
                                    strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);
                                    XrResult cr = xr->pfnCaptureAtlasEXT(xr->session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::PostFlashRequest(hwnd);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasDXR failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_DXR_atlas_capture not available");
                                }
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t col = (uint32_t)eye % cols;
                                uint32_t row = (uint32_t)eye / cols;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                if (zonesFrame) {
                                    // Zone tiles + the runtime-returned pose/fov,
                                    // verbatim (core requires pose/fov copied in).
                                    const XrView& zv = zoneViews[(uint32_t)eye < zoneViewCount ? eye : zoneViewCount - 1];
                                    projectionViews[eye].subImage.swapchain = g_zoneSwapchain.swapchain;
                                    projectionViews[eye].subImage.imageRect.offset = {
                                        (int32_t)(col * tileW), (int32_t)(row * tileH)};
                                    projectionViews[eye].subImage.imageRect.extent = {
                                        (int32_t)tileW, (int32_t)tileH};
                                    projectionViews[eye].pose = zv.pose;
                                    projectionViews[eye].fov = zv.fov;
                                } else {
                                    projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                    projectionViews[eye].subImage.imageRect.offset = {
                                        (int32_t)(col * renderW), (int32_t)(row * renderH)};
                                    projectionViews[eye].subImage.imageRect.extent = {
                                        (int32_t)renderW, (int32_t)renderH};
                                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                                    projectionViews[eye].fov = monoMode ? rawViews[0].fov : rawViews[eye].fov;
                                }
                            }
                            if (zonesFrame) ReleaseWindowSpaceImage(g_zoneSwapchain);
                            else            ReleaseSwapchainImage(*xr);

                            // Update the click-through silhouette from the same
                            // per-view matrices we just drew (so the hit mask
                            // tracks the animation, W/S dolly, billboard yaw, and
                            // the display-plane clip) — union of the outermost
                            // views, see UpdateSilhouette. Render-thread-local.
                            // Throttled to every other frame: the pass adds GPU
                            // waits, and ~30 Hz is plenty for the hit region —
                            // keeping it off the every-frame path steadies frame
                            // timing (no billboard stutter). imgW/imgH = the dims
                            // the eye render used, so the renderer's internal
                            // targets don't churn.
                            static uint32_t s_silFrame = 0;
                            if (hasGsScene && (s_silFrame++ & 1u) == 0u)
                                UpdateSilhouette(vkDevice, physDevice, graphicsQueue, renderCmdPool,
                                    targetW, targetH, windowW, windowH,
                                    viewMat, projMat, clipFar, (uint32_t)eyeCount);
                        } else {
                            rendered = false;
                        }

                        // The developer HUD info-panel was stripped from the avatar
                        // demo — see git history if a frame/mode/eye-tracking readout
                        // is needed again. The window-space-layer machinery it shared
                        // now drives only the speech-bubble (the Local2D block below).

                    }
                }

                // ── Speech bubble: a flat 2D nameplate pill in the top ~25% band,
                //    submitted as a single Local2D layer. On the zones path the tiger
                //    weaves only inside the bottom-75% zone rect, so the band is pure
                //    2D and no full-band backer is needed. RenderBubbleToTexture draws
                //    the rounded panel + balanced text into g_animBtnSwapchain (slot
                //    name predates the chrome-button strip); composited post-weave. ──
                XrCompositionLayerLocal2DDXR bubbleLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
                bool bubbleReady = false;
                if (g_animBtnReady && g_hasAnimBtnSwapchain && g_hasLocal3DZone) {
                    // The top-25% band is bandW×bandH; its aspect varies with the
                    // window. Render the rounded panel into the LARGEST band-aspect
                    // sub-rect of the (fixed) bubble texture, then map that sub-rect
                    // onto the full band below — equal scale on both axes, so corners
                    // stay round and text stays unstretched on any resize.
                    const int bandH = (windowH > 0) ? (int)(windowH * 0.25f) : 1;
                    const int bandW = (int)windowW;
                    const float bandAR = (float)bandW / (float)(bandH > 0 ? bandH : 1);
                    const float texAR  = (float)BTN_BAR_TEX_W / (float)BTN_BAR_TEX_H;
                    uint32_t subW, subH;
                    if (bandAR >= texAR) {            // band wider than texture → width-bound
                        subW = BTN_BAR_TEX_W;
                        subH = (uint32_t)((float)BTN_BAR_TEX_W / bandAR + 0.5f);
                    } else {                          // band taller → height-bound
                        subH = BTN_BAR_TEX_H;
                        subW = (uint32_t)((float)BTN_BAR_TEX_H * bandAR + 0.5f);
                    }
                    if (subW < 2) subW = 2; else if (subW > BTN_BAR_TEX_W) subW = BTN_BAR_TEX_W;
                    if (subH < 2) subH = 2; else if (subH > BTN_BAR_TEX_H) subH = BTN_BAR_TEX_H;

                    uint32_t pitch = 0;
                    const void* px = RenderBubbleToTexture(g_animBtnHud, subW, subH, g_bubbleText, &pitch);
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

                        // Visible bubble = the panel sub-rect mapped onto the FULL band
                        // (window top, edge to edge). Local2D's implicit M=0 makes the
                        // band flat 2D so the avatar stops weaving there; the panel fills
                        // it with the rounded glassy bubble.
                        bubbleLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        bubbleLayer.subImage.swapchain = g_animBtnSwapchain.swapchain;
                        bubbleLayer.subImage.imageRect.offset = {0, 0};
                        bubbleLayer.subImage.imageRect.extent = {(int32_t)subW, (int32_t)subH};
                        bubbleLayer.subImage.imageArrayIndex = 0;
                        bubbleLayer.rect.offset = {0, 0};
                        bubbleLayer.rect.extent = {bandW, bandH};

                        bubbleReady = true;
                        // Publish the full-band bubble rect so the shaped window keeps it.
                        std::lock_guard<std::mutex> bl(g_bubbleMtx);
                        g_bubbleRect = {0, 0, bandW, bandH};
                        g_bubbleVisible = true;
                    } else if (px) {
                        UnmapHud(g_animBtnHud);
                    }
                }

                // Submit frame
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                if (submitViewCount == 0) submitViewCount = 1;
                if (submitViewCount > 8) submitViewCount = 8;  // matches projectionViews[8] sizing
                if (rendered) {
                    // Submit the projection layer (the weaved avatar) plus the
                    // Local2D speech bubble. Hand-built (displayxr-common's helper
                    // has no Local2D path). ALPHA_BLEND: the avatar is always
                    // transparent (compose-under-bg), proven by the rest of the app.
                    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                    proj.space = xr->localSpace;
                    proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    proj.viewCount = submitViewCount;
                    proj.views = projectionViews;
                    if (zonesFrame) {
                        // Same XrDisplayZoneDXR instance as the locate, rig chain
                        // cleared — this makes the frame a ZONES frame: the runtime
                        // weaves the tiger views into the bottom-75% rect, composites
                        // the bubble flat in the top 25%, and auto-derives the wish
                        // (feathered bottom-75%) — no mask object, nothing chained
                        // on xrEndFrame.
                        tigerZone.next = nullptr;
                        proj.next = &tigerZone;
                    }
                    const XrCompositionLayerBaseHeader* layers[2];
                    uint32_t layerN = 0;
                    layers[layerN++] = (const XrCompositionLayerBaseHeader*)&proj;
                    if (bubbleReady) {
                        // Single Local2D bubble in the top 25%. On the zones path the
                        // tiger weaves only inside the bottom-75% zone rect (a local 3D
                        // zone confines its content to the zone by definition), so the
                        // top band is already pure 2D — the old full-band transparent
                        // backer that extended the implicit mask is redundant and gone.
                        layers[layerN++] = (const XrCompositionLayerBaseHeader*)&bubbleLayer;
                    }
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                    endInfo.layerCount = layerN;
                    endInfo.layers = layers;
                    // DXR_ZONES_VALIDATE=1: chain the strict locate/submit pairing
                    // validation (bring-up diagnostics). Default: nothing chained =
                    // auto wish.
                    static const bool s_zonesValidate = EnvZonesValidate();
                    XrDisplayZonesFrameEndInfoDXR zonesEnd = {XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR};
                    if (zonesFrame && s_zonesValidate) {
                        zonesEnd.flags = XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR;
                        zonesEnd.wishMask = XR_NULL_HANDLE;  // auto wish either way
                        endInfo.next = &zonesEnd;
                    }
                    xrEndFrame(xr->session, &endInfo);
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

    // Initialize OpenXR BEFORE creating the window — xrGetSystemProperties
    // needs only instance + system id, and returns the 3D panel's desktop
    // position (g_displayDesktopLeft/Top) that CreateAppWindow places the
    // window at (INV-1.3 ordering: instance → system → properties → window
    // → session; runtime#715).
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        CleanupOpenXR(xr);
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

    // XR_DXR_mcp_tools (#30): declare the app identity + register the base agent
    // tools now that the session exists. Must precede TryAutoLoadBundledScene so
    // the tiger's clips surface the animation tools on first auto-load
    // (ApplyAutoFitForLoadedScene_locked → UpdateMcpAnimationTools). No-op when
    // the runtime doesn't advertise the extension / the MCP gate is off.
    RegisterMcpBaseTools(xr);

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

    // ── Speech-bubble window-space layer resources ───────────────────────────
    // Own swapchain + text renderer + staging + cmd pool for the flat 2D speech
    // bubble. The g_animBtnSwapchain / g_animBtn* slot names predate the strip of
    // the old chrome buttons; the bubble reuses that window-space-layer machinery
    // (RenderBubbleToTexture draws the rounded panel + balanced text into it, with
    // the layout re-fit per resize). Only meaningful when the runtime advertises
    // Local2D (otherwise the bubble layer is never submitted).
    if (g_hasLocal3DZone) {
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
            LOG_INFO("Speech-bubble layer resources %s", ok ? "created" : "FAILED");
        } else {
            LOG_WARN("Speech-bubble layer init failed — bubble will not show");
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // ~60 Hz poll that toggles WS_EX_TRANSPARENT for shaped click-through
    // (cursor over the avatar → opaque to input; off it → pass-through).
    SetTimer(hwnd, kClickThroughTimerId, 16, nullptr);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: A/D=Move L/R  W/S=Dolly depth  Ctrl+T=Transparency  G=Edge-soften");
    LOG_INFO("          Space=Reset  V=Mode  N=Clip  K=Play/Pause");
    LOG_INFO("          B=Decoration(move/resize)  I=Capture  F11=Fullscreen  ESC=Quit");
    LOG_INFO("");

    g_inputState.viewParams.virtualDisplayHeight = kFallbackVirtualDisplayHeightM;
    g_inputState.renderingModeCount = xr.renderingModeCount;
    // Align runtime active rendering mode with app's default (mode 1 = first 3D mode).
    // The main loop's dispatch picks this up on the first frame and calls
    // xrRequestDisplayRenderingModeDXR(1); the runtime event drives xr.currentModeIndex.
    g_inputState.absoluteRenderingModeRequested = 1;
    g_inputState.animateEnabled = false; // no auto-orbit — avatar faces the viewer
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
    }

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, vkDevice, graphicsQueue,
        queueFamilyIndex, vkInstance, physDevice,
        &swapchainVkImages);

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

    // Speech-bubble window-space layer resources.
    if (g_animBtnCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, g_animBtnCmdPool, nullptr);
    if (g_animBtnStaging != VK_NULL_HANDLE) {
        if (g_animBtnStagingMapped) vkUnmapMemory(vkDevice, g_animBtnStagingMem);
        vkDestroyBuffer(vkDevice, g_animBtnStaging, nullptr);
    }
    if (g_animBtnStagingMem != VK_NULL_HANDLE) vkFreeMemory(vkDevice, g_animBtnStagingMem, nullptr);
    if (g_animBtnReady) CleanupHudRenderer(g_animBtnHud);

    // App-owned speech-bubble swapchain: destroy before CleanupOpenXR tears
    // the session down (used to live in the vendored XrSessionManager cleanup).
    if (g_animBtnSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_animBtnSwapchain.swapchain);
        g_animBtnSwapchain.swapchain = XR_NULL_HANDLE;
        g_hasAnimBtnSwapchain = false;
    }

    // App-owned tiger-zone swapchain (display-zones path), same ordering rule.
    if (g_zoneSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_zoneSwapchain.swapchain);
        g_zoneSwapchain.swapchain = XR_NULL_HANDLE;
        g_zonesActive = false;
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
