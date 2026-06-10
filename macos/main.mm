// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Vulkan OpenXR glTF 2.0 PBR model viewer with external window binding
 *
 * Renders glTF 2.0 models on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk_macos with the cube/grid renderer replaced by
 * the model_common/ModelRenderer PBR pipeline.  Features a "Load…" button overlay.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_EXT_cocoa_window_binding)
 * - Mouse drag camera, WASD/QE movement, scroll zoom
 * - XR_EXT_display_info: Kooima projection, display metrics
 * - V key cycles rendering modes via xrRequestDisplayRenderingModeEXT
 * - 0-3 keys select rendering mode directly
 * - L key or button click: NSOpenPanel to load .glb/.gltf models
 * - Tab: toggle HUD overlay, Space: reset camera, ESC: quit
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_cocoa_window_binding.h>
#include <openxr/XR_EXT_display_info.h>
#include <openxr/XR_EXT_atlas_capture.h>
#include <openxr/XR_EXT_mcp_tools.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

#include "view_params.h"
#include "display3d_view.h"
#include "camera3d_view.h"
#include "projection_depth.h"
#include "model_renderer.h"
#include "atlas_capture.h"

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Input state
// ============================================================================

struct InputState {
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    ViewParams viewParams;
    bool hudVisible = false;  // Hidden by default; toggle with Tab.
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
    bool eyeTrackingModeToggleRequested = false;
    bool loadRequested = false;
    bool teleportRequested = false;
    float teleportMouseX = 0.0f, teleportMouseY = 0.0f; // logical points

    // Smooth display-pose transition (double-click focus)
    bool transitioning = false;
    XrPosef transitionFrom = {{0,0,0,1}, {0,0,0}};
    XrPosef transitionTo   = {{0,0,0,1}, {0,0,0}};
    float transitionT = 0.0f;
    float transitionDuration = 0.45f;

    // Auto-orbit (turntable) mode
    bool animateEnabled = true;  // Always on; auto-orbit kicks in after 10 s idle.
    double lastInputTimeSec = 0.0;
    bool animationActive = false;
    bool animateToggleRequested = false;     // set by UI button

    // glTF clip playback (Phase 4): one-shot, set by 'N' / 'K', cleared by the
    // render loop after it drives ModelRenderer::cycleAnimation()/togglePaused().
    bool cycleClipRequested = false;         // 'N' → next animation clip
    bool playPauseRequested = false;         // 'K' → toggle play/pause

    // Drag-and-drop / pending file load
    std::string pendingLoadPath;

    // 'I' key: capture the rendered atlas region (cols × rows × renderW × renderH)
    // of the swapchain to <scene>_<cols>x<rows>.png in the working directory.
    // Skipped for 1×1 (mono) layouts. Useful for grabbing the SBS image
    // intended for shell launcher icons / 3D thumbnails.
    bool captureAtlasRequested = false;

    // Unified rendering mode (V key cycles, 0-8 keys select directly)
    uint32_t currentRenderingMode = 1;   // Default: mode 1 (first 3D mode)
    uint32_t renderingModeCount = 0;     // Set from xrEnumerateDisplayRenderingModesEXT
    bool renderingModeChangeRequested = false;
};

// Fallback virtual-display height in meters when no scene is loaded
// (or auto-fit fails). On scene load we replace this with a robust
// percentile-based extent — see ApplyAutoFitForLoadedScene().
static constexpr float kDefaultVirtualDisplayHeightM = 1.5f;

// Initial virtual-display height as a multiple of the model's height: the
// display-centric rig frames the (centered) model with 1.4× its height, i.e.
// ~20% headroom top and bottom — enough that the window title bar doesn't
// clip the subject.
static constexpr float kAutoFitVerticalComfort = 1.4f;

// Cached auto-fit result for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kDefaultVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static bool  g_fitValid     = false;

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;
static InputState g_input;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f;

typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

static bool g_fullscreen = false;
static NSRect g_savedWindowFrame = {};
static NSUInteger g_savedWindowStyle = 0;

// Model-viewer state
static ModelRenderer g_modelRenderer;
static std::string g_loadedFileName;

static double g_avgFrameTime = 0.0;
static uint64_t g_frameCount = 0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_renderW = 0, g_renderH = 0;
static uint32_t g_windowW = 0, g_windowH = 0;

// Atlas capture helpers (filename / Pictures dir / flash overlay / Vulkan
// readback) live in test_apps/common/atlas_capture* — see dxr_capture::*.

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

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

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
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

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// ============================================================================
// Forward decls for top-bar UI helpers (defined after CreateMacOSWindow)
// ============================================================================

struct AppXrSession;
static void UpdateTopBarButtonTitles(AppXrSession& xr);
static void ApplyAutoFitForLoadedScene();
static void UpdateMcpAnimationTools();

// ============================================================================
// Input timestamp helper
// ============================================================================

static double NowSec(void) {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

static void MarkUserInput(InputState& input) {
    input.lastInputTimeSec = NowSec();
    input.animationActive = false;
}

// Extract yaw/pitch from a quaternion (XYZ order, matches quat_from_yaw_pitch).
// Only used after a smooth pose transition completes so subsequent drag rotation
// feels natural. Ambiguous near the poles — acceptable for this demo.
static void yaw_pitch_from_quat(XrQuaternionf q, float* yaw, float* pitch) {
    // Forward vector in local is (0, 0, -1); rotate it by q to get world forward.
    float fx = 2.0f * (q.x * q.z + q.y * q.w) * -1.0f + 0.0f;
    // Reuse the cross-product form for clarity.
    float vx = 0, vy = 0, vz = -1.0f;
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    float fwdX = vx + q.w * tx + (q.y * tz - q.z * ty);
    float fwdY = vy + q.w * ty + (q.z * tx - q.x * tz);
    float fwdZ = vz + q.w * tz + (q.x * ty - q.y * tx);
    (void)fx;
    // quat_from_yaw_pitch(y,p) rotates (0,0,-1) to (-cos(p)sin(y), sin(p), -cos(p)cos(y)).
    // Inverting:  p = asin(fwdY),  y = atan2(-fwdX, -fwdZ).
    *yaw = atan2f(-fwdX, -fwdZ);
    float clampedY = fwdY;
    if (clampedY > 1.0f) clampedY = 1.0f;
    if (clampedY < -1.0f) clampedY = -1.0f;
    *pitch = asinf(clampedY);
}

// ============================================================================
// Camera movement (ported from common/input_handler)
// ============================================================================

static void UpdateCameraMovement(InputState& input, float dt, float displayHeightM) {
    if (input.resetViewRequested) {
        input.pitch = 0;
        input.viewParams = ViewParams();
        if (g_fitValid) {
            input.cameraPosX = g_fitCenter[0];
            input.cameraPosY = g_fitCenter[1];
            input.cameraPosZ = g_fitCenter[2];
            input.yaw = g_fitYaw;
            input.viewParams.virtualDisplayHeight = g_fitVHeight;
        } else {
            input.cameraPosX = input.cameraPosY = input.cameraPosZ = 0;
            input.yaw = 0;
            input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
        }
        input.resetViewRequested = false;
        input.transitioning = false;
        // Auto-orbit always on; resetting just resets the idle timer below.
        input.animationActive = false;
        input.lastInputTimeSec = NowSec();
        return;
    }

    // Smooth pose transition (double-click focus). Overrides WASD while active.
    if (input.transitioning) {
        input.transitionT += dt;
        float u = input.transitionT / input.transitionDuration;
        if (u >= 1.0f) u = 1.0f;
        // Ease-out cubic
        float invU = 1.0f - u;
        float eased = 1.0f - invU * invU * invU;
        XrPosef cur;
        display3d_pose_slerp(&input.transitionFrom, &input.transitionTo, eased, &cur);
        input.cameraPosX = cur.position.x;
        input.cameraPosY = cur.position.y;
        input.cameraPosZ = cur.position.z;
        float yaw, pitch;
        yaw_pitch_from_quat(cur.orientation, &yaw, &pitch);
        input.yaw = yaw;
        input.pitch = pitch;
        if (u >= 1.0f) input.transitioning = false;
        return;
    }

    float speed = 0.15f;
    if (displayHeightM > 0.0f) speed *= displayHeightM / 0.1f;

    // Build orientation quaternion and derive basis vectors
    XrQuaternionf ori;
    quat_from_yaw_pitch(input.yaw, input.pitch, &ori);

    float fwdX, fwdY, fwdZ, rtX, rtY, rtZ, upX, upY, upZ;
    quat_rotate_vec3(ori, 0, 0, -1, &fwdX, &fwdY, &fwdZ);
    quat_rotate_vec3(ori, 1, 0, 0, &rtX, &rtY, &rtZ);
    quat_rotate_vec3(ori, 0, 1, 0, &upX, &upY, &upZ);

    float d = speed * dt;
    if (input.keyW) { input.cameraPosX += fwdX*d; input.cameraPosY += fwdY*d; input.cameraPosZ += fwdZ*d; }
    if (input.keyS) { input.cameraPosX -= fwdX*d; input.cameraPosY -= fwdY*d; input.cameraPosZ -= fwdZ*d; }
    if (input.keyD) { input.cameraPosX += rtX*d;  input.cameraPosY += rtY*d;  input.cameraPosZ += rtZ*d; }
    if (input.keyA) { input.cameraPosX -= rtX*d;  input.cameraPosY -= rtY*d;  input.cameraPosZ -= rtZ*d; }
    if (input.keyE) { input.cameraPosX += upX*d;  input.cameraPosY += upY*d;  input.cameraPosZ += upZ*d; }
    if (input.keyQ) { input.cameraPosX -= upX*d;  input.cameraPosY -= upY*d;  input.cameraPosZ -= upZ*d; }

    // Auto-orbit: if enabled and user has been idle > 10s, slowly yaw the display.
    double idleFor = NowSec() - input.lastInputTimeSec;
    input.animationActive = (input.animateEnabled && idleFor > 10.0);
    if (input.animationActive) {
        float rate = 6.2831853f / 20.0f; // one revolution per 20 seconds
        input.yaw += rate * dt;
    }
}

// ============================================================================
// HUD overlay (simple NSView with CoreText)
// ============================================================================

@interface HudOverlayView : NSView
@property (nonatomic, strong) NSString *hudText;
@end

@implementation HudOverlayView

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    if (!_hudText) return;
    // No backdrop fill — the enclosing NSVisualEffectView provides frosted
    // vibrancy that auto-adapts to whatever is behind the window.
    NSDictionary *attrs = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor labelColor]
    };
    NSRect textRect = NSInsetRect(self.bounds, 10, 8);
    [_hudText drawInRect:textRect withAttributes:attrs];
}

@end

static HudOverlayView   *g_hudView = nil;      // text view
static NSVisualEffectView *g_hudBackdrop = nil;  // frosted wrapper sized to hudView

// ============================================================================
// Top-bar overlay (Open / Auto-Orbit / Mode buttons) + reticle
// ============================================================================

static NSView   *g_topBar = nil;
static NSButton *g_openButton = nil;
static NSButton *g_modeButton = nil;
static NSButton *g_animButton = nil;   // right-justified clip button; hidden w/o clips
static NSView   *g_animButtonBackdrop = nil;
static NSView   *g_reticleView = nil;

// Translucent dark background view used behind the top bar.
@interface TopBarBackdropView : NSView
@end
@implementation TopBarBackdropView
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)r {
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.55] set];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.08] set];
    NSRect hr = NSMakeRect(0, 0, self.bounds.size.width, 1);
    NSRectFill(hr);
}
@end

// Non-interactive crosshair at the center of the window (aim reference for double-click).
// Drawn as a dark outline + bright core so it reads against any background.
@interface ReticleView : NSView
@end
@implementation ReticleView
- (BOOL)isFlipped { return NO; }
- (NSView*)hitTest:(NSPoint)p { (void)p; return nil; } // never steal clicks
- (void)drawRect:(NSRect)r {
    (void)r;
    NSRect b = self.bounds;
    CGFloat cx = b.size.width * 0.5f, cy = b.size.height * 0.5f;
    // Dark outline for contrast on light backgrounds
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.75] set];
    NSRectFill(NSMakeRect(cx - 5.5, cy - 0.75, 11, 1.5));
    NSRectFill(NSMakeRect(cx - 0.75, cy - 5.5, 1.5, 11));
    // Bright core for contrast on dark backgrounds
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.95] set];
    NSRectFill(NSMakeRect(cx - 4.5, cy, 9, 1));
    NSRectFill(NSMakeRect(cx, cy - 4.5, 1, 9));
}
@end

static void OpenLoadDialog() {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Load 3D Model"];
        [panel setMessage:@"Select a 3D model (glTF / STL / OBJ / FBX / USD)"];

        if (@available(macOS 11.0, *)) {
            NSMutableArray<UTType *> *types = [NSMutableArray array];
            for (NSString *ext in @[@"glb", @"gltf", @"stl", @"obj", @"fbx",
                                    @"usdz", @"usd", @"usda", @"usdc"]) {
                UTType *t = [UTType typeWithFilenameExtension:ext];
                if (t) [types addObject:t];
            }
            if (types.count > 0) {
                [panel setAllowedContentTypes:types];
            }
        }

        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) {
                const char *path = [[url path] UTF8String];
                std::string pathStr(path);
                if (model_validate_file(pathStr)) {
                    LOG_INFO("Loading model: %s", path);
                    if (g_modelRenderer.loadModel(path)) {
                        g_loadedFileName = model_basename(pathStr);
                        LOG_INFO("Model loaded: %s (%s)", g_loadedFileName.c_str(),
                            model_filesize_str(pathStr).c_str());
                        ApplyAutoFitForLoadedScene();
                    } else {
                        LOG_ERROR("Failed to load model: %s", path);
                        NSAlert *alert = [[NSAlert alloc] init];
                        [alert setMessageText:@"Failed to load model file"];
                        [alert setInformativeText:@"The file may be corrupt or unsupported."];
                        [alert runModal];
                    }
                }
            }
        }
    }
}

// ============================================================================
// macOS Window + Metal Layer
// ============================================================================

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n { (void)n; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)windowWillClose:(NSNotification *)n { (void)n; g_running = false; }
@end

@interface MetalView : NSView
@end

@implementation MetalView
- (CALayer*)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    return layer;
}
- (BOOL)wantsLayer { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent *)event {
    MarkUserInput(g_input);
    if ([event clickCount] >= 2) {
        NSPoint loc = [event locationInWindow];
        g_input.teleportRequested = true;
        g_input.teleportMouseX = (float)loc.x;
        g_input.teleportMouseY = (float)loc.y;
    }
    [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent *)event {
    MarkUserInput(g_input);
    g_input.yaw -= (float)[event deltaX] * 0.005f;
    // pitch += because the renderer Y-mirrors the world internally
    // (model_renderer.cpp); without this, mouse-drag-up would tilt the
    // camera DOWN. cube_handle apps go the other way (-= deltaY)
    // because they Y-flip at rasterization (negative VkViewport.height),
    // not at view stage.
    g_input.pitch += (float)[event deltaY] * 0.005f;
    float maxPitch = 1.5f;
    if (g_input.pitch > maxPitch) g_input.pitch = maxPitch;
    if (g_input.pitch < -maxPitch) g_input.pitch = -maxPitch;
}

- (void)scrollWheel:(NSEvent *)event {
    MarkUserInput(g_input);
    float delta = (float)[event scrollingDeltaY] * 0.02f;
    g_input.viewParams.scaleFactor += delta * 0.5f;
    if (g_input.viewParams.scaleFactor < 0.1f) g_input.viewParams.scaleFactor = 0.1f;
}

- (void)keyDown:(NSEvent *)event {
    MarkUserInput(g_input);
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 'w': case 'W': g_input.keyW = true; break;
        case 'a': case 'A': g_input.keyA = true; break;
        case 's': case 'S': g_input.keyS = true; break;
        case 'd': case 'D': g_input.keyD = true; break;
        case 'e': case 'E': g_input.keyE = true; break;
        case 'q': case 'Q': g_input.keyQ = true; break;
        case 'v': case 'V':
            // Cycle through all rendering modes
            if (g_input.renderingModeCount > 0) {
                g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
            }
            g_input.renderingModeChangeRequested = true;
            break;
        case 'm': case 'M':
            g_input.animateToggleRequested = true;
            break;
        case 'n': case 'N':   // next glTF animation clip
            g_input.cycleClipRequested = true;
            break;
        case 'k': case 'K':   // play/pause the active clip
            g_input.playPauseRequested = true;
            break;
        case 'c': case 'C':
            g_input.cameraMode = !g_input.cameraMode;
            break;
        case 'i': case 'I':
            g_input.captureAtlasRequested = true;
            break;
        case 't': case 'T':
            g_input.eyeTrackingModeToggleRequested = true;
            break;
        case 'l': case 'L':
            g_input.loadRequested = true;
            break;
        case '-': case '_': {
            float v = g_input.viewParams.ipdFactor - 0.1f;
            if (v < 0.1f) v = 0.1f;
            g_input.viewParams.ipdFactor = v;
            g_input.viewParams.parallaxFactor = v;
            break;
        }
        case '=': case '+': {
            float v = g_input.viewParams.ipdFactor + 0.1f;
            if (v > 1.0f) v = 1.0f;
            g_input.viewParams.ipdFactor = v;
            g_input.viewParams.parallaxFactor = v;
            break;
        }
        case ' ':
            g_input.resetViewRequested = true;
            break;
        case '0':
            g_input.currentRenderingMode = 0;
            g_input.renderingModeChangeRequested = true;
            break;
        case '1':
            if (g_input.renderingModeCount > 1) g_input.currentRenderingMode = 1;
            g_input.renderingModeChangeRequested = true;
            break;
        case '2':
            if (g_input.renderingModeCount > 2) g_input.currentRenderingMode = 2;
            g_input.renderingModeChangeRequested = true;
            break;
        case '3':
            if (g_input.renderingModeCount > 3) g_input.currentRenderingMode = 3;
            g_input.renderingModeChangeRequested = true;
            break;
        case '\t':
            g_input.hudVisible = !g_input.hudVisible;
            break;
        case 27: // ESC
            g_running = false;
            break;
    }
}

- (void)keyUp:(NSEvent *)event {
    MarkUserInput(g_input);
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 'w': case 'W': g_input.keyW = false; break;
        case 'a': case 'A': g_input.keyA = false; break;
        case 's': case 'S': g_input.keyS = false; break;
        case 'd': case 'D': g_input.keyD = false; break;
        case 'e': case 'E': g_input.keyE = false; break;
        case 'q': case 'Q': g_input.keyQ = false; break;
    }
}

// Drag-and-drop: accept .glb and .gltf files
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([[pb types] containsObject:NSPasteboardTypeFileURL]) {
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]] options:nil];
    for (NSURL *url in urls) {
        if (![url isFileURL]) continue;
        NSString *path = [url path];
        NSString *ext = [[path pathExtension] lowercaseString];
        if ([ext isEqualToString:@"glb"] || [ext isEqualToString:@"gltf"]) {
            g_input.pendingLoadPath = std::string([path UTF8String]);
            return YES;
        }
    }
    return NO;
}

- (void)flagsChanged:(NSEvent *)event {
    // Cmd+Ctrl+F = fullscreen toggle
    NSUInteger flags = [event modifierFlags];
    (void)flags;
}
@end

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

static bool CreateMacOSWindow(uint32_t width, uint32_t height) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    NSRect frame = NSMakeRect(100, 100, width, height);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
    g_window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:style backing:NSBackingStoreBuffered defer:NO];
    [g_window setTitle:@"DisplayXR 3D Model Viewer"];
    [g_window setDelegate:delegate];
    [g_window center];

    g_metalView = [[MetalView alloc] initWithFrame:frame];
    [g_window setContentView:g_metalView];
    [g_window makeKeyAndOrderFront:nil];
    [g_window makeFirstResponder:g_metalView];

    // Accept drag-and-drop of .glb / .gltf files
    [g_metalView registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    // Add HUD overlay — frosted backdrop + text view inside
    NSRect hudFrame = NSMakeRect(8, 8, 320, 520);
    g_hudBackdrop = [[NSVisualEffectView alloc] initWithFrame:hudFrame];
    [g_hudBackdrop setMaterial:NSVisualEffectMaterialHUDWindow];
    [g_hudBackdrop setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [g_hudBackdrop setState:NSVisualEffectStateActive];
    [g_hudBackdrop setWantsLayer:YES];
    g_hudBackdrop.layer.cornerRadius = 8.0;
    g_hudBackdrop.layer.masksToBounds = YES;
    [g_metalView addSubview:g_hudBackdrop];

    g_hudView = [[HudOverlayView alloc] initWithFrame:g_hudBackdrop.bounds];
    [g_hudView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [g_hudBackdrop addSubview:g_hudView];

    // --- Top bar (Open / Mode) — transparent so the buttons sit directly
    // over the rendered content (no frosted panel hiding the top of the scene).
    const CGFloat barH = 48.0;
    NSRect barFrame = NSMakeRect(0, height - barH, width, barH);
    NSView *topBar = [[NSView alloc] initWithFrame:barFrame];
    [topBar setWantsLayer:YES];
    [[topBar layer] setBackgroundColor:[[NSColor clearColor] CGColor]];
    [topBar setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    g_topBar = topBar;
    [g_metalView addSubview:g_topBar];

    const CGFloat btnH = 32.0, btnY = (barH - btnH) * 0.5f;
    const CGFloat gap = 10.0;
    CGFloat x = 12.0;
    CGFloat openW = 96.0, modeW = 220.0;

    // Helper: wrap a button in a glassy NSVisualEffectView backdrop matching
    // the HUD's HUDWindow material so the controls have the same look.
    NSView * (^makeGlassyButton)(NSRect, NSString*, SEL, NSButton **) =
        ^NSView *(NSRect frame, NSString *title, SEL act, NSButton **outBtn) {
        NSVisualEffectView *bd = [[NSVisualEffectView alloc] initWithFrame:frame];
        [bd setMaterial:NSVisualEffectMaterialHUDWindow];
        [bd setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
        [bd setState:NSVisualEffectStateActive];
        [bd setWantsLayer:YES];
        bd.layer.cornerRadius = 6.0;
        bd.layer.masksToBounds = YES;
        NSButton *b = [[NSButton alloc] initWithFrame:bd.bounds];
        [b setTitle:title];
        [b setBezelStyle:NSBezelStyleInline];
        [b setBordered:NO];
        [b setTarget:nil];
        [b setAction:act];
        [b setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [bd addSubview:b];
        if (outBtn) *outBtn = b;
        return bd;
    };

    NSView *openBd = makeGlassyButton(NSMakeRect(x, btnY, openW, btnH),
                                       @"Open…", @selector(openButtonClicked:),
                                       &g_openButton);
    [g_topBar addSubview:openBd];
    x += openW + gap;

    NSView *modeBd = makeGlassyButton(NSMakeRect(x, btnY, modeW, btnH),
                                       @"Mode: —", @selector(modeButtonClicked:),
                                       &g_modeButton);
    [g_topBar addSubview:modeBd];

    // Animation button — right-justified (pinned to the right edge via
    // NSViewMinXMargin so it stays put on resize). Hidden until a model with
    // clips loads (see UpdateTopBarButtonTitles). Click = next clip (N-key).
    const CGFloat animW = 160.0;
    NSView *animBd = makeGlassyButton(NSMakeRect(width - animW - 12.0, btnY, animW, btnH),
                                       @"", @selector(animationButtonClicked:),
                                       &g_animButton);
    [animBd setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [animBd setHidden:YES];
    g_animButtonBackdrop = animBd;
    [g_topBar addSubview:animBd];

    // --- Reticle (non-interactive center crosshair) ---
    const CGFloat retSize = 20.0;
    NSRect retFrame = NSMakeRect((width - retSize) * 0.5f, (height - retSize) * 0.5f, retSize, retSize);
    g_reticleView = [[ReticleView alloc] initWithFrame:retFrame];
    [g_reticleView setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin];
    [g_metalView addSubview:g_reticleView];

    [NSApp activateIgnoringOtherApps:YES];
    LOG_INFO("macOS window created (%ux%u)", width, height);
    return true;
}

// Button action handlers (added as category on NSApplication)
@interface NSApplication (TopBarActions)
- (void)openButtonClicked:(id)sender;
- (void)modeButtonClicked:(id)sender;
- (void)animationButtonClicked:(id)sender;
@end

@implementation NSApplication (TopBarActions)
- (void)openButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    g_input.loadRequested = true;
}
- (void)modeButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    if (g_input.renderingModeCount > 0) {
        g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
    }
    g_input.renderingModeChangeRequested = true;
}
- (void)animationButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    g_input.cycleClipRequested = true;   // next clip (N-key equivalent)
}
@end

// NOTE: UpdateTopBarButtonTitles() body lives after the AppXrSession struct
// definition further below, since it accesses its members.

static void PumpMacOSEvents() {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

static void ToggleBorderlessFullscreen() {
    if (g_fullscreen) {
        [g_window setStyleMask:g_savedWindowStyle];
        [g_window setFrame:g_savedWindowFrame display:YES animate:NO];
        [g_window setLevel:NSNormalWindowLevel];
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen");
    } else {
        g_savedWindowStyle = [g_window styleMask];
        g_savedWindowFrame = [g_window frame];
        NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
        [g_window setStyleMask:NSWindowStyleMaskBorderless];
        [g_window setFrame:[screen frame] display:YES animate:NO];
        [g_window setLevel:NSStatusWindowLevel];
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen");
    }
}

// ============================================================================
// OpenXR Session (ported from cube_handle_vk_macos)
// ============================================================================

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    char systemName[256] = {};

    // Swapchain
    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    // Display info from XR_EXT_display_info
    bool hasDisplayInfoExt = false;
    bool hasCocoaWindowBinding = false;
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerX = 0, nominalViewerY = 0, nominalViewerZ = 0.5f;
    float recommendedViewScaleX = 0.5f, recommendedViewScaleY = 1.0f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;

    // Eye tracking
    float eyePositions[8][3] = {};  // [view][x,y,z] — raw per-eye positions in display space
    bool eyeTrackingActive = false;
    bool isEyeTracking = false;
    uint32_t activeEyeTrackingMode = 0;
    uint32_t supportedEyeTrackingModes = 0;

    // Function pointers
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT = nullptr;
    PFN_xrRequestEyeTrackingModeEXT pfnRequestEyeTrackingModeEXT = nullptr;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT = nullptr;

    // XR_EXT_atlas_capture (W6 of #396): runtime-owned 'I' key capture.
    bool hasAtlasCaptureExt = false;
    PFN_xrCaptureAtlasEXT pfnCaptureAtlasEXT = nullptr;

    // XR_EXT_mcp_tools (#22): app-defined agent tools on the runtime-hosted
    // per-process MCP server. The whole path is inert when the extension or
    // the MCP capability gate is absent — never load-bearing.
    bool hasMcpToolsExt = false;
    PFN_xrSetMCPAppInfoEXT pfnSetMCPAppInfo = nullptr;
    PFN_xrRegisterMCPToolEXT pfnRegisterMCPTool = nullptr;
    PFN_xrUnregisterMCPToolEXT pfnUnregisterMCPTool = nullptr;
    PFN_xrGetMCPToolCallArgsEXT pfnGetMCPToolCallArgs = nullptr;
    PFN_xrSubmitMCPToolResultEXT pfnSubmitMCPToolResult = nullptr;
    bool mcpToolsReady = false;           // appId declared + base tools registered
    bool mcpAnimToolsRegistered = false;  // list/play/stop_animation currently live

    // Enumerated rendering mode info
    uint32_t renderingModeCount = 0;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE] = {};
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    uint32_t renderingModeTileColumns[8] = {};  // atlas tile layout (v12)
    uint32_t renderingModeTileRows[8] = {};

    // Max views the runtime may return from xrLocateViews, taken from
    // xrEnumerateViewConfigurationViews at session init. Some runtimes (e.g.
    // sim_display on macOS) report the union across all rendering modes, so
    // this is >= 2 even for PRIMARY_STEREO.
    uint32_t maxViewCount = 2;

    void* windowHandle = nullptr;  // unused on macOS, kept for compatibility
};

// Session reachable from the model-load paths (they all funnel through
// ApplyAutoFitForLoadedScene, which is xr-free) so they can flip the
// animation-tool registration. Set in main() right after CreateSession.
static AppXrSession* g_xrForMcp = nullptr;

static void UpdateTopBarButtonTitles(AppXrSession& xr) {
    if (g_modeButton) {
        const char *name = "Unknown";
        if (xr.renderingModeCount > 0 &&
            g_input.currentRenderingMode < xr.renderingModeCount &&
            xr.renderingModeNames[g_input.currentRenderingMode][0] != '\0') {
            name = xr.renderingModeNames[g_input.currentRenderingMode];
        }
        [g_modeButton setTitle:[NSString stringWithFormat:@"Mode: %s", name]];
    }
}

// Refresh the right-justified animation button: shown only when the model has
// clips; label = current clip name, or "Paused". Called on load + N/K events.
// xr-free so the load hook (ApplyAutoFitForLoadedScene) can call it.
static void UpdateAnimButton() {
    if (!g_animButton || !g_animButtonBackdrop) return;
    std::string clip; int ci, cn; float ct, cd; bool playing;
    if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing)) {
        [g_animButtonBackdrop setHidden:NO];
        [g_animButton setTitle:playing ? [NSString stringWithUTF8String:clip.c_str()] : @"Paused"];
    } else {
        [g_animButtonBackdrop setHidden:YES];
    }
}

// ★ XR_EXT_mcp_tools late registration (#22): the animation tools exist only
// while a model with clips is loaded; they are unregistered when the model is
// replaced by one without. Each transition makes the runtime broadcast the MCP
// tools/list_changed notification, so agents connected BEFORE a load see the
// tools appear/disappear live. Called from every successful load path (they
// all funnel through ApplyAutoFitForLoadedScene).
static void UpdateMcpAnimationTools() {
    AppXrSession* xr = g_xrForMcp;
    if (!xr || !xr->mcpToolsReady || !xr->pfnRegisterMCPTool || !xr->pfnUnregisterMCPTool)
        return;
    const bool want = g_modelRenderer.hasAnimations();
    if (want == xr->mcpAnimToolsRegistered) return;

    if (want) {
        XrMCPToolInfoEXT listTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
        listTool.name = "list_animations";
        listTool.description =
            "List the loaded model's animation clips: index, name and duration in "
            "seconds, plus the active clip index and whether playback is running. "
            "Only available while a model with animation clips is loaded.";
        listTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r1 = xr->pfnRegisterMCPTool(xr->session, &listTool);

        XrMCPToolInfoEXT playTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
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
        XrResult r2 = xr->pfnRegisterMCPTool(xr->session, &playTool);

        XrMCPToolInfoEXT stopTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
        stopTool.name = "stop_animation";
        stopTool.description =
            "Pause animation playback, freezing the model at its current pose. "
            "Resume with play_animation.";
        stopTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r3 = xr->pfnRegisterMCPTool(xr->session, &stopTool);

        xr->mcpAnimToolsRegistered = XR_SUCCEEDED(r1) || XR_SUCCEEDED(r2) || XR_SUCCEEDED(r3);
        LOG_INFO("XR_EXT_mcp_tools: animation tools registered (%d clip(s)) [%d %d %d]",
                 g_modelRenderer.animationCount(), r1, r2, r3);
    } else {
        xr->pfnUnregisterMCPTool(xr->session, "list_animations");
        xr->pfnUnregisterMCPTool(xr->session, "play_animation");
        xr->pfnUnregisterMCPTool(xr->session, "stop_animation");
        xr->mcpAnimToolsRegistered = false;
        LOG_INFO("XR_EXT_mcp_tools: animation tools unregistered (model has no clips)");
    }
}

// Forward declarations for OpenXR functions (same as cube_handle_vk_macos)
static bool InitializeOpenXR(AppXrSession& xr);
static bool GetVulkanGraphicsRequirements(AppXrSession& xr);
static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance);
static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);
static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice);
static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);
static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& exts, VkDevice& device, VkQueue& queue);
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex);
static bool CreateSpaces(AppXrSession& xr);
static bool CreateSwapchains(AppXrSession& xr);
static void PollEvents(AppXrSession& xr);
static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState);
static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex);
static void ReleaseSwapchainImage(AppXrSession& xr);
static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount);
static void CleanupOpenXR(AppXrSession& xr);

// ============================================================================
// OpenXR implementation (abbreviated — same logic as cube_handle_vk_macos)
// ============================================================================

static bool InitializeOpenXR(AppXrSession& xr) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& ext : exts) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasCocoaWindowBinding = true;
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(ext.extensionName, XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME) == 0) xr.hasAtlasCaptureExt = true;
        if (strcmp(ext.extensionName, XR_EXT_MCP_TOOLS_EXTENSION_NAME) == 0) xr.hasMcpToolsExt = true;
    }

    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasCocoaWindowBinding) enabled.push_back(XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasAtlasCaptureExt) enabled.push_back(XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME);
    if (xr.hasMcpToolsExt) enabled.push_back(XR_EXT_MCP_TOOLS_EXTENSION_NAME);

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "DisplayXRModelViewerMacOS", sizeof(ci.applicationInfo.applicationName));
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName));
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &xr.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

    { XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
      xrGetSystemProperties(xr.instance, xr.systemId, &sp);
      memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName)); }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
        XrEyeTrackingModeCapabilitiesEXT ec = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT};
        di.next = &ec; sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.recommendedViewScaleX = di.recommendedViewScaleX;
            xr.recommendedViewScaleY = di.recommendedViewScaleY;
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerX = di.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = di.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)ec.supportedModes;
        }
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
        if (xr.supportedEyeTrackingModes != 0)
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT", (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    // XR_EXT_atlas_capture (W6 of #396): resolve the runtime-owned capture entry.
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasEXT", (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
        LOG_INFO("xrCaptureAtlasEXT: %s", xr.pfnCaptureAtlasEXT ? "resolved" : "NULL");
    }

    // XR_EXT_mcp_tools (#22): resolve the agent-tool entry points. Tools are
    // registered after session create (CreateSession) and dispatched from
    // PollEvents.
    if (xr.hasMcpToolsExt) {
        xrGetInstanceProcAddr(xr.instance, "xrSetMCPAppInfoEXT", (PFN_xrVoidFunction*)&xr.pfnSetMCPAppInfo);
        xrGetInstanceProcAddr(xr.instance, "xrRegisterMCPToolEXT", (PFN_xrVoidFunction*)&xr.pfnRegisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrUnregisterMCPToolEXT", (PFN_xrVoidFunction*)&xr.pfnUnregisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrGetMCPToolCallArgsEXT", (PFN_xrVoidFunction*)&xr.pfnGetMCPToolCallArgs);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitMCPToolResultEXT", (PFN_xrVoidFunction*)&xr.pfnSubmitMCPToolResult);
        const bool resolved = xr.pfnSetMCPAppInfo && xr.pfnRegisterMCPTool &&
            xr.pfnUnregisterMCPTool && xr.pfnGetMCPToolCallArgs && xr.pfnSubmitMCPToolResult;
        LOG_INFO("XR_EXT_mcp_tools entry points: %s", resolved ? "resolved" : "NULL");
    } else {
        LOG_INFO("XR_EXT_mcp_tools: not advertised by runtime");
    }

    LOG_INFO("OpenXR initialized: %s", xr.systemName);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames;
    std::vector<const char*> extPtrs;
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') extNames.push_back(n);
        s = e + 1;
    }}
    // Add portability enumeration for MoltenVK
    extNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "DisplayXRModelViewerMacOS";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)extPtrs.size();
    ci.ppEnabledExtensionNames = extPtrs.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& pd) {
    PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    LOG_INFO("GPU: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') storage.push_back(n);
        s = e + 1;
    }}
    // Add portability subset for MoltenVK
    storage.push_back("VK_KHR_portability_subset");
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
    }
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
    const std::vector<const char*>& exts, VkDevice& dev, VkQueue& queue) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = qfi; qi.queueCount = 1; qi.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures features = {};
    features.shaderInt64 = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
    VkDevice dev, uint32_t qfi) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    XrCocoaWindowBindingCreateInfoEXT macBinding = {(XrStructureType)XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT};
    macBinding.viewHandle = (__bridge void*)g_metalView;
    if (xr.hasCocoaWindowBinding && g_metalView) {
        vkBinding.next = &macBinding;
        LOG_INFO("Using XR_EXT_cocoa_window_binding");
    }

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = &vkBinding; si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));

    // XR_EXT_mcp_tools (#22): declare identity + register the base agent
    // tools. The appId MUST match `id` in
    // displayxr/model_viewer_handle_vk_macos.displayxr.json (INV-10.1).
    // Failure is non-fatal by design — the MCP capability gate may simply be
    // off on this machine; the viewer runs identically without an agent
    // surface. The animation tools are NOT registered here: they appear only
    // once a model with clips loads (UpdateMcpAnimationTools).
    if (xr.hasMcpToolsExt && xr.pfnSetMCPAppInfo && xr.pfnRegisterMCPTool) {
        XrMCPAppInfoEXT mcpAppInfo = {XR_TYPE_MCP_APP_INFO_EXT};
        strncpy(mcpAppInfo.appId, "modelviewer", sizeof(mcpAppInfo.appId) - 1);
        XrResult ar = xr.pfnSetMCPAppInfo(xr.session, &mcpAppInfo);
        if (XR_SUCCEEDED(ar)) {
            XrMCPToolInfoEXT loadTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
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
            XrResult t1 = xr.pfnRegisterMCPTool(xr.session, &loadTool);

            XrMCPToolInfoEXT statusTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
            statusTool.name = "get_status";
            statusTool.description =
                "Read the viewer's live state: loaded model file and primitive count, "
                "animation clip count + active clip + playing flag, camera orbit "
                "(azimuth/elevation in degrees, world position, zoom factor), active "
                "rendering-mode index, and whether the XR session is running.";
            statusTool.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult t2 = xr.pfnRegisterMCPTool(xr.session, &statusTool);

            XrMCPToolInfoEXT orbitTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
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
            XrResult t3 = xr.pfnRegisterMCPTool(xr.session, &orbitTool);

            XrMCPToolInfoEXT frameTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
            frameTool.name = "frame_model";
            frameTool.description =
                "Reset the camera to the loaded model's auto-fit framed pose (same as "
                "pressing Space): centers the model with comfortable headroom and "
                "restores zoom to 1. Requires a model to be loaded.";
            frameTool.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult t4 = xr.pfnRegisterMCPTool(xr.session, &frameTool);

            xr.mcpToolsReady = true;
            LOG_INFO("XR_EXT_mcp_tools: appId=modelviewer load_model=%d get_status=%d "
                     "set_orbit=%d frame_model=%d", t1, t2, t3, t4);
        } else {
            LOG_INFO("XR_EXT_mcp_tools: appId not accepted (%d) — no agent surface", ar);
        }
    }

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoEXT> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, tiles=%ux%u, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY,
                        xr.renderingModeTileColumns[i], xr.renderingModeTileRows[i],
                        modes[i].hardwareDisplay3D);
                }
            }
        }
    }

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));

    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, views.data());
    xr.maxViewCount = viewCount;
    LOG_INFO("View config: %u views reported by runtime", viewCount);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());

    int64_t selectedFmt = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0];
    for (auto f : fmts) {
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
    }

    // Size the swapchain at init from the largest atlas any rendering mode
    // could produce when the app is running full-screen — atlas dims per
    // mode are (cols × scaleX × displayPixelW) × (rows × scaleY × displayPixelH).
    // For sim_display and Leia SR this collapses to the panel resolution
    // (max(cols × scaleX) ≤ 1 across all their advertised modes). The atlas
    // the app actually writes per frame is smaller — driven by the live
    // window size — but the swapchain has to accommodate full-screen so the
    // app can resize / fullscreen at any time without reallocating. Falls
    // back to recommended × (2,1) if display info is unavailable.
    uint32_t w = views[0].recommendedImageRectWidth * 2;
    uint32_t h = views[0].recommendedImageRectHeight;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        w = xr.displayPixelWidth;
        h = xr.displayPixelHeight;
        if (xr.renderingModeCount > 0) {
            uint32_t maxAtlasW = 0, maxAtlasH = 0;
            for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] *
                                          xr.renderingModeScaleX[i] *
                                          (double)xr.displayPixelWidth);
                uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] *
                                          xr.renderingModeScaleY[i] *
                                          (double)xr.displayPixelHeight);
                if (aw > maxAtlasW) maxAtlasW = aw;
                if (ah > maxAtlasH) maxAtlasH = ah;
            }
            if (maxAtlasW > w) w = maxAtlasW;
            if (maxAtlasH > h) h = maxAtlasH;
        }
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;

    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, selectedFmt);
    return true;
}

// ============================================================================
// XR_EXT_mcp_tools dispatch (#22)
// ============================================================================
// Minimal JSON helpers — hand-rolled on purpose, matching the runtime
// reference adopter (cube_handle_metal_macos): tool args are tiny one-level
// objects, so a JSON dependency isn't warranted.

static std::string JsonEscape(const std::string& s) {
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
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract "key":"value" (string) with backslash-escape handling, incl. a
// basic \uXXXX → UTF-8 decode (no surrogate pairs — file paths don't need
// them). False when the key is absent or its value is not a string.
static bool JsonGetString(const char* json, const char* key, std::string& out) {
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

// Extract "key": <number>. False when absent or not numeric (strtod refuses
// a leading quote, so string values correctly fail).
static bool JsonGetNumber(const char* json, const char* key, double& out) {
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

// Dispatch one agent tool call. Runs on the main loop (called from
// PollEvents), where app state is naturally consistent — no locking. EVERY
// call is answered — success=XR_FALSE + {"error":…} for bad args — because an
// unanswered call only fails to the agent after the runtime's ~5 s timeout.
static void HandleMcpToolCall(AppXrSession& xr, const XrEventDataMCPToolCallEXT* call) {
    // Two-call idiom: argsSize from the event is the required capacity incl. NUL.
    std::string args;
    if (xr.pfnGetMCPToolCallArgs && call->argsSize > 0) {
        std::vector<char> buf(call->argsSize, '\0');
        uint32_t needed = 0;
        if (XR_SUCCEEDED(xr.pfnGetMCPToolCallArgs(xr.session, call->callId,
                                                  (uint32_t)buf.size(), &needed, buf.data())))
            args.assign(buf.data());
    }
    const char* a = args.c_str();
    std::string result;
    XrBool32 ok = XR_TRUE;
    char buf[1024];

    if (strcmp(call->toolName, "load_model") == 0) {
        std::string path;
        if (!JsonGetString(a, "path", path) || path.empty()) {
            ok = XR_FALSE;
            result = "{\"error\":\"missing required string argument 'path'\"}";
        } else if (!model_validate_file(path)) {
            ok = XR_FALSE;
            result = "{\"error\":\"not a readable supported model file: " +
                     JsonEscape(path) + "\"}";
        } else if (!g_modelRenderer.loadModel(path.c_str())) {
            ok = XR_FALSE;
            result = "{\"error\":\"failed to load (corrupt or unsupported): " +
                     JsonEscape(path) + "\"}";
        } else {
            g_loadedFileName = model_basename(path);
            LOG_INFO("Model loaded via MCP: %s (%s)", g_loadedFileName.c_str(),
                     model_filesize_str(path).c_str());
            // Re-frames the camera, refreshes the clip button, and registers/
            // unregisters the agent animation tools for the new model.
            ApplyAutoFitForLoadedScene();
            snprintf(buf, sizeof(buf),
                     "{\"file\":\"%s\",\"primitives\":%u,\"animation_count\":%d}",
                     JsonEscape(g_loadedFileName).c_str(),
                     g_modelRenderer.primitiveCount(), g_modelRenderer.animationCount());
            result = buf;
        }
    } else if (strcmp(call->toolName, "get_status") == 0) {
        std::string clip; int ci = -1, cn = 0; float ct = 0, cd = 0; bool playing = false;
        const bool hasClip = g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing);
        const float azDeg = fmodf(g_input.yaw * 57.29578f, 360.0f);
        const float elDeg = g_input.pitch * 57.29578f;
        std::string clipJson = hasClip ? "\"" + JsonEscape(clip) + "\"" : "null";
        snprintf(buf, sizeof(buf),
                 "{\"file\":\"%s\",\"loaded\":%s,\"primitives\":%u,"
                 "\"animation_count\":%d,\"active_animation\":%d,"
                 "\"active_animation_name\":%s,\"animation_playing\":%s,"
                 "\"camera\":{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,"
                 "\"position\":[%.3f,%.3f,%.3f],\"zoom\":%.2f},"
                 "\"rendering_mode\":%u,\"session_running\":%s}",
                 JsonEscape(g_loadedFileName).c_str(),
                 g_modelRenderer.hasModel() ? "true" : "false",
                 g_modelRenderer.primitiveCount(),
                 g_modelRenderer.animationCount(),
                 hasClip ? ci : -1, clipJson.c_str(),
                 (hasClip && playing) ? "true" : "false",
                 azDeg, elDeg,
                 g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                 g_input.viewParams.scaleFactor,
                 g_input.currentRenderingMode,
                 xr.sessionRunning ? "true" : "false");
        result = buf;
    } else if (strcmp(call->toolName, "set_orbit") == 0) {
        double az, el, zm;
        bool any = false;
        if (JsonGetNumber(a, "azimuth_deg", az)) {
            g_input.yaw = (float)(az * 0.0174532925);
            any = true;
        }
        if (JsonGetNumber(a, "elevation_deg", el)) {
            if (el > 85.0) el = 85.0;
            if (el < -85.0) el = -85.0;
            g_input.pitch = (float)(el * 0.0174532925);
            any = true;
        }
        if (JsonGetNumber(a, "zoom", zm)) {
            if (zm < 0.1) zm = 0.1;
            if (zm > 10.0) zm = 10.0;
            g_input.viewParams.scaleFactor = (float)zm;
            any = true;
        }
        if (!any) {
            ok = XR_FALSE;
            result = "{\"error\":\"provide at least one of azimuth_deg, elevation_deg, zoom\"}";
        } else {
            MarkUserInput(g_input);  // agent input is input: reset the auto-orbit idle timer
            snprintf(buf, sizeof(buf),
                     "{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,\"zoom\":%.2f}",
                     g_input.yaw * 57.29578f, g_input.pitch * 57.29578f,
                     g_input.viewParams.scaleFactor);
            result = buf;
        }
    } else if (strcmp(call->toolName, "frame_model") == 0) {
        if (!g_modelRenderer.hasModel()) {
            ok = XR_FALSE;
            result = "{\"error\":\"no model loaded — call load_model first\"}";
        } else {
            g_input.resetViewRequested = true;  // applied by UpdateCameraMovement next frame
            result = "{\"framed\":true}";
        }
    } else if (strcmp(call->toolName, "list_animations") == 0) {
        const int n = g_modelRenderer.animationCount();
        std::string clips = "[";
        for (int i = 0; i < n; i++) {
            std::string nm; float dur = 0;
            g_modelRenderer.getAnimationInfo(i, nm, dur);
            snprintf(buf, sizeof(buf), "%s{\"index\":%d,\"name\":\"%s\",\"duration_s\":%.2f}",
                     i ? "," : "", i, JsonEscape(nm).c_str(), dur);
            clips += buf;
        }
        clips += "]";
        snprintf(buf, sizeof(buf), ",\"active_index\":%d,\"playing\":%s}",
                 g_modelRenderer.activeAnimation(),
                 (g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused()) ? "true" : "false");
        result = "{\"animations\":" + clips + buf;
    } else if (strcmp(call->toolName, "play_animation") == 0) {
        const int n = g_modelRenderer.animationCount();
        int target = -1;
        double idx; std::string nm;
        if (JsonGetNumber(a, "index", idx)) {
            target = (int)idx;
            if (target < 0 || target >= n) {
                ok = XR_FALSE;
                snprintf(buf, sizeof(buf), "{\"error\":\"index out of range (0..%d)\"}", n - 1);
                result = buf;
            }
        } else if (JsonGetString(a, "name", nm)) {
            for (int i = 0; i < n && target < 0; i++) {
                std::string c; float d;
                g_modelRenderer.getAnimationInfo(i, c, d);
                if (c == nm) target = i;
            }
            if (target < 0) {
                ok = XR_FALSE;
                result = "{\"error\":\"no clip named '" + JsonEscape(nm) +
                         "' — see list_animations\"}";
            }
        } else {
            target = g_modelRenderer.activeAnimation();  // resume the active clip
            if (target < 0) target = 0;
        }
        if (ok == XR_TRUE) {
            if (n == 0) {
                ok = XR_FALSE;
                result = "{\"error\":\"the loaded model has no animation clips\"}";
            } else {
                if (target != g_modelRenderer.activeAnimation())
                    g_modelRenderer.setActiveAnimation(target);
                g_modelRenderer.setPaused(false);
                UpdateAnimButton();
                std::string c; float d = 0;
                g_modelRenderer.getAnimationInfo(target, c, d);
                snprintf(buf, sizeof(buf),
                         "{\"playing\":\"%s\",\"index\":%d,\"duration_s\":%.2f}",
                         JsonEscape(c).c_str(), target, d);
                result = buf;
            }
        }
    } else if (strcmp(call->toolName, "stop_animation") == 0) {
        g_modelRenderer.setPaused(true);
        UpdateAnimButton();
        std::string c; float d = 0;
        const int active = g_modelRenderer.activeAnimation();
        if (active >= 0) g_modelRenderer.getAnimationInfo(active, c, d);
        snprintf(buf, sizeof(buf), "{\"playing\":false,\"paused_clip\":%s%s%s}",
                 active >= 0 ? "\"" : "", active >= 0 ? JsonEscape(c).c_str() : "null",
                 active >= 0 ? "\"" : "");
        result = buf;
    } else {
        ok = XR_FALSE;
        result = "{\"error\":\"unhandled tool\"}";
    }

    if (xr.pfnSubmitMCPToolResult)
        xr.pfnSubmitMCPToolResult(xr.session, call->callId, ok, result.c_str());
}

static void PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = ssc->state;
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = xr.viewConfigType;
                xrBeginSession(xr.session, &bi);
                xr.sessionRunning = true;
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING) {
                xr.exitRequested = true;
            }
        } else if (event.type == (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT) {
            // Runtime (or another client / shell) switched rendering mode on us.
            auto* rmc = (XrEventDataRenderingModeChangedEXT*)&event;
            if (rmc->currentModeIndex < xr.renderingModeCount) {
                g_input.currentRenderingMode = rmc->currentModeIndex;
                UpdateTopBarButtonTitles(xr);
                LOG_INFO("Rendering mode changed: %u -> %u (%s)",
                    rmc->previousModeIndex, rmc->currentModeIndex,
                    xr.renderingModeNames[rmc->currentModeIndex]);
            }
        } else if (event.type == (XrStructureType)XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT) {
            // An agent invoked one of our XR_EXT_mcp_tools tools (#22).
            HandleMcpToolCall(xr, (const XrEventDataMCPToolCallEXT*)&event);
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(xr.session, nullptr, &fs);
    if (XR_FAILED(r)) return false;
    return XR_SUCCEEDED(xrBeginFrame(xr.session, nullptr));
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 1000000000;
    return XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &wi));
}

static void ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
}

static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = 1; ei.layers = layers;
    xrEndFrame(xr.session, &ei);
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.localSpace) xrDestroySpace(xr.localSpace);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
}

// ============================================================================
// Placeholder rendering (clear to dark gray when no scene loaded)
// ============================================================================

static void RenderPlaceholder(VkDevice dev, VkQueue queue, VkCommandPool pool,
                               VkImage image, uint32_t w, uint32_t h,
                               float yaw, float pitch) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Tint color based on camera direction so drag-rotation gives visual feedback
    float ny = (yaw / 3.14159f) * 0.5f + 0.5f;   // 0..1 over ±π
    float np = (pitch / 1.5f) * 0.5f + 0.5f;       // 0..1 over ±1.5 rad
    VkClearColorValue cc = {{0.05f + ny * 0.15f, 0.08f + np * 0.12f, 0.15f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

// ============================================================================
// Bundled-scene auto-load
// ============================================================================

static std::string ExeDir() {
    char buf[PATH_MAX]; uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return "";
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved)) return std::string(buf);
    return std::string(dirname(resolved));
}

static bool FileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Compute robust scene bounds (5th–95th percentile per axis) and set the
// display rig pose + vHeight to frame the model. Display orientation is
// kept identity (forward = world −Z): a model's canonical front is its
// authored +Z, which glTF places facing the viewer; the user can rotate
// with mouse drag from a predictable starting pose.
static void ApplyAutoFitForLoadedScene() {
    UpdateAnimButton();        // show/hide + label the clip button for the new model
    UpdateMcpAnimationTools(); // (un)register the agent animation tools (#22)
    float center[3], extent[3];
    // Robust AABB (5th–95th percentile per axis): center for the rig position,
    // extent[1] for the height fit. Percentile trim rejects stray outlier
    // vertices that would otherwise inflate the frame.
    if (g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent)) {
        g_fitCenter[0] = center[0];
        g_fitCenter[1] = center[1];
        g_fitCenter[2] = center[2];
        float vh = extent[1] * kAutoFitVerticalComfort;
        if (!(vh > 1e-3f)) vh = kDefaultVirtualDisplayHeightM; // degenerate scene
        g_fitVHeight = vh;

        // glTF models are authored front-facing (+Z toward the viewer), so a
        // yaw scan isn't needed — start at yaw=0 and let the user rotate.
        g_fitYaw = 0.0f;

        g_fitValid = true;
        LOG_INFO("Auto-fit: center=(%.3f, %.3f, %.3f) extent=(%.3f, %.3f, %.3f) vHeight=%.3f yaw=%.0fdeg",
                 center[0], center[1], center[2],
                 extent[0], extent[1], extent[2], vh, g_fitYaw * 57.2957795f);
    } else {
        g_fitValid = false;
    }

    g_input.cameraPosX = g_fitValid ? g_fitCenter[0] : 0.0f;
    g_input.cameraPosY = g_fitValid ? g_fitCenter[1] : 0.0f;
    g_input.cameraPosZ = g_fitValid ? g_fitCenter[2] : 0.0f;
    g_input.yaw = g_fitValid ? g_fitYaw : 0.0f;
    g_input.pitch = 0.0f;
    g_input.viewParams.virtualDisplayHeight = g_fitValid ? g_fitVHeight : kDefaultVirtualDisplayHeightM;
    g_input.viewParams.scaleFactor = 1.0f;
    // Treat scene load as a fresh user interaction so the auto-orbit idle
    // timer restarts. Without this, an asset loaded after the 10s idle
    // threshold starts rotating immediately on first display.
    MarkUserInput(g_input);

    // glTF uses a +Y-up, right-handed coordinate system natively.
    // ModelRenderer::updateUniforms negates the Y row of proj_mat to match the
    // +Y-up convention. No runtime view-stage flips needed.
}

static void TryAutoLoadBundledScene() {
    std::string dir = ExeDir();
    if (dir.empty()) return;
    std::string path = dir + "/sample.glb";
    if (!FileExists(path)) {
        LOG_INFO("No bundled model at %s (skipping auto-load)", path.c_str());
        return;
    }
    if (!model_validate_file(path)) return;
    LOG_INFO("Auto-loading bundled model: %s", path.c_str());
    if (g_modelRenderer.loadModel(path.c_str())) {
        g_loadedFileName = model_basename(path);
        LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(), model_filesize_str(path).c_str());
        ApplyAutoFitForLoadedScene();
    } else {
        LOG_WARN("Auto-load failed for %s", path.c_str());
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== DisplayXR 3D Model Viewer (Vulkan) + External macOS Window ===");

    // Initialize rendering mode from env var (legacy fallback)
    {
        const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
        if (mode_str) {
            if (strcmp(mode_str, "anaglyph") == 0) g_input.currentRenderingMode = 1;
            else if (strcmp(mode_str, "sbs") == 0) g_input.currentRenderingMode = 2;
            else if (strcmp(mode_str, "blend") == 0) g_input.currentRenderingMode = 3;
            else g_input.currentRenderingMode = 1; // default to anaglyph
        }
    }

    // Step 1: Create macOS window
    g_windowW = 1280; g_windowH = 720;
    if (!CreateMacOSWindow(g_windowW, g_windowH)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    { NSSize cs = [[g_window contentView] bounds].size;
      CGFloat bs = [g_window backingScaleFactor];
      g_windowW = (uint32_t)(cs.width * bs);
      g_windowH = (uint32_t)(cs.height * bs);
      LOG_INFO("Window drawable: %ux%u", g_windowW, g_windowH); }

    // Step 2: Initialize OpenXR
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // Try to find sim_display_set_output_mode
    { void *rtHandle = NULL;
      uint32_t ic = _dyld_image_count();
      for (uint32_t i = 0; i < ic; i++) {
          const char *name = _dyld_get_image_name(i);
          if (name && strstr(name, "openxr_displayxr")) {
              rtHandle = dlopen(name, RTLD_NOLOAD); break;
          }
      }
      if (rtHandle) g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)dlsym(rtHandle, "sim_display_set_output_mode");
      LOG_INFO("sim_display hot-reload: %s", g_pfnSetOutputMode ? "available" : "not available"); }

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    // Model-load paths can now flip the agent animation-tool registration
    // (XR_EXT_mcp_tools late registration, #22). Set before the bundled-scene
    // auto-load below so it too funnels through UpdateMcpAnimationTools.
    g_xrForMcp = &xr;

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    // Enumerate swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    { uint32_t count = xr.swapchain.imageCount;
      swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    // Initialize model renderer
    { uint32_t rw = xr.swapchain.width;   // Full width — mono uses entire swapchain
      uint32_t rh = xr.swapchain.height;
      if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue, queueFamilyIndex, rw, rh))
          LOG_WARN("model renderer init failed");
    }

    // Command pool for placeholder rendering
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      ci.queueFamilyIndex = queueFamilyIndex;
      vkCreateCommandPool(vkDevice, &ci, nullptr, &cmdPool); }

    g_input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
    g_input.nominalViewerZ = xr.nominalViewerZ;
    g_input.renderingModeCount = xr.renderingModeCount;
    // Align the runtime's active rendering mode with the app's default
    // (currentRenderingMode = 1, the first 3D mode) at startup. The sim display
    // boots in 2D (mode 0); without this the display stays 2D until the user
    // toggles. The main-loop dispatch holds this request until the session is
    // running, so it isn't issued to a not-yet-begun session and lost.
    g_input.renderingModeChangeRequested = true;
    g_input.lastInputTimeSec = NowSec();

    // Reflect initial state in top-bar buttons.
    UpdateTopBarButtonTitles(xr);

    // Try loading the bundled sample.glb model (copied next to the exe by CMake).
    TryAutoLoadBundledScene();

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom  DblClick=Focus");
    LOG_INFO("          -/= Depth  Space=Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          L/Open=Load  Tab=HUD  ESC=Quit  (.glb/.gltf also accept drag-and-drop)");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PumpMacOSEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        g_frameCount++;
        g_avgFrameTime = g_avgFrameTime * 0.95 + deltaTime * 0.05;

        // Handle load request (from L key or Open button)
        if (g_input.loadRequested) {
            g_input.loadRequested = false;
            OpenLoadDialog();
        }

        // Handle drag-and-drop load
        if (!g_input.pendingLoadPath.empty()) {
            std::string p = g_input.pendingLoadPath;
            g_input.pendingLoadPath.clear();
            if (model_validate_file(p)) {
                LOG_INFO("Loading dropped model: %s", p.c_str());
                if (g_modelRenderer.loadModel(p.c_str())) {
                    g_loadedFileName = model_basename(p);
                    ApplyAutoFitForLoadedScene();
                }
            }
        }

        // Handle Auto-Orbit toggle (M key or button)
        if (g_input.animateToggleRequested) {
            g_input.animateToggleRequested = false;
            g_input.animateEnabled = !g_input.animateEnabled;
            g_input.lastInputTimeSec = NowSec(); // don't snap-start
            UpdateTopBarButtonTitles(xr);
        }

        UpdateCameraMovement(g_input, deltaTime, xr.displayHeightM);
        // Clip playback (N=next, K=play/pause), applied before the per-frame
        // advance so this frame reflects it.
        if (g_input.cycleClipRequested || g_input.playPauseRequested) {
            if (g_input.cycleClipRequested) { g_input.cycleClipRequested = false; g_modelRenderer.cycleAnimation(); }
            if (g_input.playPauseRequested) { g_input.playPauseRequested = false; g_modelRenderer.togglePaused(); }
            UpdateAnimButton();   // refresh label (clip name ↔ "Paused")
        }
        // Advance node/TRS animation once per frame (no-op for static models).
        g_modelRenderer.updateAnimation(deltaTime);

        // Handle rendering mode change (V=cycle, 0-3=direct, Mode button, or the
        // startup default-mode request). Held until the session is running so the
        // request reaches a begun session rather than being dropped (this handler
        // runs before PollEvents, which is what begins the session).
        if (g_input.renderingModeChangeRequested && xr.sessionRunning) {
            g_input.renderingModeChangeRequested = false;
            if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
                xr.pfnRequestDisplayRenderingModeEXT(xr.session, g_input.currentRenderingMode);
            }
            UpdateTopBarButtonTitles(xr);
        }

        // Handle eye tracking mode toggle
        if (g_input.eyeTrackingModeToggleRequested) {
            g_input.eyeTrackingModeToggleRequested = false;
            if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
                xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            }
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                std::vector<XrCompositionLayerProjectionView> projectionViews;
                bool rendered = false;

                if (frameState.shouldRender) {
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    XrViewEyeTrackingStateEXT eyeTrackingState = {};
                    eyeTrackingState.type = (XrStructureType)XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT;
                    viewState.next = &eyeTrackingState;

                    uint32_t runtimeViewCount = xr.maxViewCount > 0 ? xr.maxViewCount : 2;
                    if (runtimeViewCount > 8) runtimeViewCount = 8;
                    XrView views[8] = {};
                    for (uint32_t v = 0; v < runtimeViewCount; v++) views[v].type = XR_TYPE_VIEW;

                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState,
                        runtimeViewCount, &runtimeViewCount, views);
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                        // --- Per-frame mode metadata ---
                        uint32_t modeViewCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount)
                            ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                        if (modeViewCount < 1) modeViewCount = 1;
                        if (modeViewCount > runtimeViewCount) modeViewCount = runtimeViewCount;
                        bool display3D = (xr.renderingModeCount > 0)
                            ? xr.renderingModeDisplay3D[g_input.currentRenderingMode] : true;
                        bool monoMode = !display3D;
                        uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.renderingModeTileColumns[g_input.currentRenderingMode] > 0)
                            ? xr.renderingModeTileColumns[g_input.currentRenderingMode]
                            : (monoMode ? 1u : 2u);
                        uint32_t tileRows = (xr.renderingModeCount > 0 && xr.renderingModeTileRows[g_input.currentRenderingMode] > 0)
                            ? xr.renderingModeTileRows[g_input.currentRenderingMode]
                            : 1u;

                        int eyeCount = monoMode ? 1 : (int)modeViewCount;

                        // Collect raw eye positions for every view this mode uses.
                        std::vector<XrVector3f> rawEyePos(modeViewCount);
                        for (uint32_t v = 0; v < modeViewCount; v++)
                            rawEyePos[v] = views[v].pose.position;

                        // HUD exposes the first two for display; log everything up to 8.
                        for (uint32_t v = 0; v < modeViewCount && v < 8; v++) {
                            xr.eyePositions[v][0] = rawEyePos[v].x;
                            xr.eyePositions[v][1] = rawEyePos[v].y;
                            xr.eyePositions[v][2] = rawEyePos[v].z;
                        }
                        xr.isEyeTracking = (eyeTrackingState.isTracking == XR_TRUE);
                        xr.activeEyeTrackingMode = (uint32_t)eyeTrackingState.activeMode;

                        // Mono mode: centroid of all runtime views.
                        if (monoMode) {
                            XrVector3f c = {0, 0, 0};
                            for (uint32_t v = 0; v < modeViewCount; v++) {
                                c.x += rawEyePos[v].x; c.y += rawEyePos[v].y; c.z += rawEyePos[v].z;
                            }
                            float inv = 1.0f / (float)modeViewCount;
                            c.x *= inv; c.y *= inv; c.z *= inv;
                            rawEyePos.assign(1, c);
                        }

                        XrPosef cameraPose;
                        quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &cameraPose.orientation);
                        // ModelRenderer Y-mirrors the world inside updateUniforms (see comment
                        // in model_renderer.cpp). The off-axis Kooima projection assumes the
                        // mirror is reflected in the displayPose passed to display3d_compute_view,
                        // so negate Y here to keep the eye-vs-display geometry consistent.
                        cameraPose.position = {g_input.cameraPosX, -g_input.cameraPosY, g_input.cameraPosZ};

                        // nominalViewer in render frame (Y mirrored) — used for
                        // parallax-factor lerp, must match the eye/cameraPose frame.
                        XrVector3f nominalViewer = {xr.nominalViewerX, -xr.nominalViewerY, xr.nominalViewerZ};

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (used for the projection viewport per eye and for the 'I'
                        // capture region) follow as cols × renderW × rows × renderH.
                        // The swapchain was sized at creation time to fit the
                        // largest atlas across all advertised modes, so no clamp
                        // is needed here.
                        float scaleX = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleX[g_input.currentRenderingMode] : 1.0f;
                        float scaleY = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleY[g_input.currentRenderingMode] : 1.0f;
                        uint32_t renderW = (uint32_t)((double)g_windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)g_windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;
                        g_renderW = renderW; g_renderH = renderH;

                        // Per-view Kooima pose + projection — one entry per view in this
                        // multiview mode (1 for mono, 2 for stereo, 4 for quad, etc.).
                        std::vector<Display3DView> eyeViews((size_t)eyeCount);
                        bool hasKooima = (xr.displayWidthM > 0 && xr.displayHeightM > 0);
                        if (hasKooima) {
                            float dispPxW = xr.displayPixelWidth > 0 ? (float)xr.displayPixelWidth : (float)xr.swapchain.width;
                            float dispPxH = xr.displayPixelHeight > 0 ? (float)xr.displayPixelHeight : (float)xr.swapchain.height;
                            float pxSizeX = xr.displayWidthM / dispPxW;
                            float pxSizeY = xr.displayHeightM / dispPxH;
                            float winW_m = (float)g_windowW * pxSizeX;
                            float winH_m = (float)g_windowH * pxSizeY;

                            // Window-relative Kooima: compute eye offset from window center
                            float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                            if (g_window != nil) {
                                NSRect winFrame = [g_window frame];
                                NSScreen *screen_ns = [g_window screen] ?: [NSScreen mainScreen];
                                NSRect screenFrame = [screen_ns frame];
                                float winCenterX = (winFrame.origin.x - screenFrame.origin.x) + winFrame.size.width / 2.0f;
                                float winCenterY = (winFrame.origin.y - screenFrame.origin.y) + winFrame.size.height / 2.0f;
                                float dispCenterX = screenFrame.size.width / 2.0f;
                                float dispCenterY = screenFrame.size.height / 2.0f;
                                CGFloat backingScale = [g_window backingScaleFactor];
                                float pxSizeXBacking = pxSizeX / (float)backingScale;
                                float pxSizeYBacking = pxSizeY / (float)backingScale;
                                eyeOffsetX = (winCenterX - dispCenterX) * pxSizeXBacking;
                                eyeOffsetY = (winCenterY - dispCenterY) * pxSizeYBacking;
                            }
                            for (auto& e : rawEyePos) { e.x -= eyeOffsetX; e.y -= eyeOffsetY; }
                            // ModelRenderer Y-mirrors the world; cameraPose Y is negated
                            // below at the display3d_compute_views boundary. Eyes must
                            // live in the same render frame so the asymmetric Kooima
                            // projection's eye-vs-display geometry stays consistent —
                            // otherwise vertical eye parallax comes out inverted.
                            for (auto& e : rawEyePos) { e.y = -e.y; }

                            Display3DScreen screen = {winW_m, winH_m};
                            Display3DTunables tunables;
                            tunables.ipd_factor = g_input.viewParams.ipdFactor;
                            tunables.parallax_factor = g_input.viewParams.parallaxFactor;
                            tunables.perspective_factor = g_input.viewParams.perspectiveFactor;
                            tunables.virtual_display_height = g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

                            // ZDP-relative clip planes (computed per-eye from eye.z inside
                            // display3d_compute_view): near = ez - near_offset, far =
                            // ez + far_offset, with the offsets given as ABSOLUTE distances
                            // in virtual-display-height (vH) units. Scales with the virtual
                            // display (zoom) so large models don't clip. macOS has no
                            // transparent-bg mode, so far_offset keeps a large recede band.
                            const float vH = tunables.virtual_display_height;
                            const float near_offset = vH;
                            const float far_offset  = 1000.0f * vH;

                            display3d_compute_views(
                                rawEyePos.data(), (uint32_t)eyeCount, &nominalViewer,
                                &screen, &tunables, &cameraPose,
                                near_offset, far_offset, /*vulkan_flip_y=*/0, eyeViews.data());
                            // displayxr::math emits a GL ([-1,1] clip-z) projection; this is a
                            // Vulkan renderer, so remap each per-view projection to [0,1]
                            // (reproduces modelviewer's prior direct-[0,1] output). [#396 W3]
                            for (uint32_t _v = 0; _v < (uint32_t)eyeCount; _v++)
                                convert_projection_gl_to_zero_to_one(eyeViews[_v].projection_matrix);
                        }

                        // Double-click focus: ray from CENTER physical eyes through the
                        // physical mouse location on the display surface, pick nearest surface,
                        // then smoothly move & re-orient the virtual display to face back
                        // along the ray.
                        if (g_input.teleportRequested && hasKooima) {
                            g_input.teleportRequested = false;
                            NSSize viewSize = [[g_window contentView] bounds].size;
                            float ndcX = 2.0f * g_input.teleportMouseX / (float)viewSize.width - 1.0f;
                            // Cocoa locationInWindow has y=0 at the BOTTOM of the window.
                            // OpenGL/+Y-up NDC also has y=-1 at the bottom, so this maps
                            // directly with no negation. (The Windows demo negates because
                            // Win32 mouse y=0 is at the TOP.)
                            float ndcY = 2.0f * g_input.teleportMouseY / (float)viewSize.height - 1.0f;

                            // Build a center-eye Display3DView from the averaged processed
                            // display-space eye so unprojection is truly from the viewpoint
                            // midpoint (not from the left eye, which is off by ~IPD/2).
                            float dispPxW2 = xr.displayPixelWidth > 0 ? (float)xr.displayPixelWidth : (float)xr.swapchain.width;
                            float dispPxH2 = xr.displayPixelHeight > 0 ? (float)xr.displayPixelHeight : (float)xr.swapchain.height;
                            float winW_m2 = (float)g_windowW * (xr.displayWidthM / dispPxW2);
                            float winH_m2 = (float)g_windowH * (xr.displayHeightM / dispPxH2);
                            Display3DScreen screen2 = {winW_m2, winH_m2};
                            Display3DTunables tunables2;
                            tunables2.ipd_factor = g_input.viewParams.ipdFactor;
                            tunables2.parallax_factor = g_input.viewParams.parallaxFactor;
                            tunables2.perspective_factor = g_input.viewParams.perspectiveFactor;
                            tunables2.virtual_display_height = g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

                            XrVector3f centerEyeDisp = {0, 0, 0};
                            for (const auto& sv : eyeViews) {
                                centerEyeDisp.x += sv.eye_display.x;
                                centerEyeDisp.y += sv.eye_display.y;
                                centerEyeDisp.z += sv.eye_display.z;
                            }
                            float invN = 1.0f / (float)eyeViews.size();
                            centerEyeDisp.x *= invN;
                            centerEyeDisp.y *= invN;
                            centerEyeDisp.z *= invN;
                            // eye_display is processed_eye * (perspective_factor * virtual_display_height / winH_m);
                            // invert so display3d_compute_view can re-apply it consistently.
                            float m2v_post = tunables2.virtual_display_height / winH_m2;
                            float es = tunables2.perspective_factor * m2v_post;
                            XrVector3f centerEyeProcessed = (es != 0.0f)
                                ? (XrVector3f){centerEyeDisp.x / es, centerEyeDisp.y / es, centerEyeDisp.z / es}
                                : centerEyeDisp;
                            // Use a real-world-frame displayPose for picking — the unproject
                            // ray needs to be in the same frame as the model (un-Y-flipped
                            // world). cameraPose has its Y negated for the renderer's view-
                            // stage Y mirror; using it here would put rayOrigin in a mirror
                            // frame and pickSurface would intersect against the mesh in the
                            // wrong frame.
                            XrPosef cameraPoseWorld = cameraPose;
                            cameraPoseWorld.position.y = g_input.cameraPosY;
                            Display3DView centerView;
                            const float pick_vH = tunables2.virtual_display_height;
                            display3d_compute_view(&centerEyeProcessed, &screen2, &tunables2,
                                                   &cameraPoseWorld, pick_vH, 1000.0f * pick_vH, &centerView);

                            XrVector3f rayOriginV, rayDirV;
                            display3d_unproject_ndc_to_ray(ndcX, ndcY,
                                centerView.view_matrix, centerView.projection_matrix,
                                &rayOriginV, &rayDirV);

                            float rayOrigin[3] = {rayOriginV.x, rayOriginV.y, rayOriginV.z};
                            float rayDir[3]    = {rayDirV.x,    rayDirV.y,    rayDirV.z};
                            float hitPos[3];
                            if (g_modelRenderer.pickSurface(rayOrigin, rayDir, hitPos)) {
                                // Both endpoints stored in WORLD frame (the same frame as
                                // g_input.cameraPosX/Y/Z) so the slerp interpolates
                                // consistently. cameraPose has its Y negated for the
                                // display3d_compute_view boundary; we mustn't store that
                                // negated copy as the slerp's "from" pose, otherwise the
                                // mid-transition lerp drifts vertically toward 2·cy − y.
                                XrPosef fromWorld;
                                fromWorld.orientation = cameraPose.orientation;
                                fromWorld.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};
                                XrPosef target;
                                target.position = {hitPos[0], hitPos[1], hitPos[2]};
                                target.orientation = cameraPose.orientation;
                                g_input.transitionFrom = fromWorld;
                                g_input.transitionTo = target;
                                g_input.transitionT = 0.0f;
                                g_input.transitioning = true;
                                LOG_INFO("Focus on surface (%.3f, %.3f, %.3f)",
                                    hitPos[0], hitPos[1], hitPos[2]);
                            }
                        } else if (g_input.teleportRequested) {
                            g_input.teleportRequested = false; // consume without Kooima
                        }

                        rendered = true;
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            projectionViews.assign((size_t)eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                            std::vector<std::array<float, 16>> viewMat((size_t)eyeCount);
                            std::vector<std::array<float, 16>> projMat((size_t)eyeCount);
                            std::vector<std::pair<uint32_t, uint32_t>> tileOffsets((size_t)eyeCount);
                            for (int eye = 0; eye < eyeCount; eye++) {
                                int srcView = eye < (int)runtimeViewCount ? eye : 0;
                                if (hasKooima) {
                                    memcpy(viewMat[eye].data(), eyeViews[eye].view_matrix, sizeof(float) * 16);
                                    memcpy(projMat[eye].data(), eyeViews[eye].projection_matrix, sizeof(float) * 16);
                                    views[srcView].pose.position = eyeViews[eye].eye_world;
                                    views[srcView].pose.orientation = cameraPose.orientation;
                                } else {
                                    mat4_view_from_xr_pose(viewMat[eye].data(), views[srcView].pose);
                                    mat4_from_xr_fov(projMat[eye].data(), views[srcView].fov, 0.01f, 100.0f);
                                }

                                // Tile-aware viewport: row-major eye layout in the atlas.
                                // For mono (cols=rows=1) this collapses to (0, 0).
                                uint32_t tileX = (uint32_t)(eye % (int)tileColumns);
                                uint32_t tileY = (uint32_t)(eye / (int)tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;
                                tileOffsets[eye] = {vpX, vpY};

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                                projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = views[srcView].pose;
                                projectionViews[eye].fov = hasKooima ? eyeViews[eye].fov : views[srcView].fov;
                            }

                            // Render model or placeholder
                            VkImage targetImage = swapchainImages[imageIndex].image;
                            VkFormat swapFormat = (VkFormat)xr.swapchain.format;

                            if (g_modelRenderer.hasModel()) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    g_modelRenderer.renderEye(
                                        targetImage, swapFormat,
                                        xr.swapchain.width, xr.swapchain.height,
                                        tileOffsets[eye].first, tileOffsets[eye].second,
                                        renderW, renderH,
                                        viewMat[eye].data(), projMat[eye].data());
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, cmdPool,
                                    targetImage, xr.swapchain.width, xr.swapchain.height,
                                    g_input.yaw, g_input.pitch);
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasEXT
                            // (XR_EXT_atlas_capture, W6 of #396). The runtime owns
                            // the readback — no app-side staging texture. Skipped
                            // for mono (1×1). The prefix has no ".png"; the runtime
                            // appends "_atlas.png".
                            if (g_input.captureAtlasRequested) {
                                g_input.captureAtlasRequested = false;
                                uint32_t cols = tileColumns > 0 ? tileColumns : 1u;
                                uint32_t rows = tileRows > 0 ? tileRows : 1u;
                                if (!g_modelRenderer.hasModel()) {
                                    LOG_WARN("Capture skipped: no model loaded");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                } else if (xr.pfnCaptureAtlasEXT &&
                                           xr.session != XR_NULL_HANDLE) {
                                    // Strip extension from model filename
                                    // (e.g. "sample.glb" → "sample").
                                    auto dot = g_loadedFileName.find_last_of('.');
                                    std::string stem = (dot == std::string::npos)
                                        ? g_loadedFileName
                                        : g_loadedFileName.substr(0, dot);
                                    if (stem.empty()) stem = "scene";
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;
                                    strncpy(info.pathPrefix, prefix.c_str(),
                                            XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1);
                                    XrResult cr = xr.pfnCaptureAtlasEXT(xr.session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::TriggerCaptureFlash(
                                            (__bridge void*)g_metalView);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasEXT failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_EXT_atlas_capture not available");
                                }
                            }

                            ReleaseSwapchainImage(xr);
                        } else {
                            rendered = false;
                        }
                    }
                }

                if (rendered) {
                    EndFrame(xr, frameState.predictedDisplayTime,
                        projectionViews.data(), (uint32_t)projectionViews.size());
                } else {
                    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
                    ei.displayTime = frameState.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    ei.layerCount = 0; ei.layers = nullptr;
                    xrEndFrame(xr.session, &ei);
                }
            }
        } else {
            usleep(100000);
        }

        // Update HUD
        g_hudUpdateTimer += deltaTime;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                if (g_input.hudVisible && g_hudView != nil) {
                    double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                    NSString *sceneInfo = g_modelRenderer.hasModel()
                        ? [NSString stringWithFormat:@"Model: %s", g_loadedFileName.c_str()]
                        : @"No model loaded (press L)";

                    int depthPct = (int)(g_input.viewParams.ipdFactor * 100.0f + 0.5f);
                    const char *orbitLabel = g_input.animateEnabled
                        ? (g_input.animationActive ? "ON (running)" : "ON (idle countdown)")
                        : "OFF";
                    uint32_t activeViewCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount)
                        ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                    NSMutableString *eyesStr = [NSMutableString string];
                    for (uint32_t v = 0; v < activeViewCount && v < 8; v++) {
                        [eyesStr appendFormat:@"View %u: (%.3f, %.3f, %.3f)\n", v,
                            xr.eyePositions[v][0], xr.eyePositions[v][1], xr.eyePositions[v][2]];
                    }
                    NSString *text = [NSString stringWithFormat:
                        @"%s\nSession: %d\n"
                        "Mode: %s (%s, %u view%s)\n"
                        "%@\n"
                        "Depth/IPD: %d%%  Zoom: %.2fx  Auto-Orbit: %s\n"
                        "FPS: %.0f (%.1f ms)\n"
                        "Render: %ux%u  Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "%@"
                        "Vdisplay: (%.2f, %.2f, %.2f)\n"
                        "\nWASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom\n"
                        "DblClick=Focus  -/= Depth  Space=Reset  N=Clip  K=Play/Pause\n"
                        "M=Auto-Orbit  V=Mode  L=Load  Tab=HUD  ESC=Quit",
                        xr.systemName, (int)xr.sessionState,
                        (xr.renderingModeCount > 0 && xr.renderingModeNames[g_input.currentRenderingMode][0] != '\0') ? xr.renderingModeNames[g_input.currentRenderingMode] : "Unknown",
                        (xr.renderingModeCount > 0 ? (xr.renderingModeDisplay3D[g_input.currentRenderingMode] ? "3D" : "2D") : "3D"),
                        activeViewCount, activeViewCount == 1 ? "" : "s",
                        sceneInfo,
                        depthPct, g_input.viewParams.scaleFactor, orbitLabel,
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH, g_windowW, g_windowH,
                        xr.displayWidthM, xr.displayHeightM,
                        eyesStr,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ];
                    g_hudView.hudText = text;
                    // Auto-size the frosted backdrop to fit the text; inner view auto-resizes.
                    NSDictionary *attrs = @{
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
                    };
                    NSRect textBounds = [text boundingRectWithSize:NSMakeSize(420, CGFLOAT_MAX)
                                         options:NSStringDrawingUsesLineFragmentOrigin
                                         attributes:attrs];
                    CGFloat pad = 20.0;
                    NSRect hudFrame = NSMakeRect(8, 8,
                        ceilf(textBounds.size.width + pad),
                        ceilf(textBounds.size.height + pad));
                    [g_hudBackdrop setFrame:hudFrame];
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudBackdrop setHidden:NO];
                } else if (g_hudBackdrop != nil) {
                    [g_hudBackdrop setHidden:YES];
                }
            }
        }
    }

    // Clean exit
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        xrRequestExitSession(xr.session);
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr); usleep(10000);
        }
    }

    LOG_INFO("=== Shutting down ===");
    g_xrForMcp = nullptr;  // session is going away; stop touching MCP tools
    g_modelRenderer.cleanup();
    if (cmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
    CleanupOpenXR(xr);
    // MoltenVK may throw std::system_error ("mutex lock failed") during device/instance
    // destruction due to internal threading cleanup.  Catch and ignore since we're exiting.
    try {
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
    } catch (const std::exception& e) {
        LOG_WARN("Vulkan cleanup exception (ignored): %s", e.what());
    }
    LOG_INFO("Application shutdown complete");
    return 0;
}
