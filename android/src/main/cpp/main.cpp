// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// model_viewer_vk_android entry point. Android port of the DisplayXR
// Gaussian-splat demo: reuses cube_handle_vk_android's OpenXR-Android
// harness (loader → instance → Vulkan device → stereo swapchains →
// session), but swaps the cube pipeline for the 3dgs_common ModelRenderer,
// which renders the bundled sample.glb scene per eye via 8 compute
// shaders. The runtime's Leia DP weaves the two views to the 3D panel.

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/input.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>  // display rendering-mode enumerate/request
#include <openxr/XR_EXT_view_rig.h>      // runtime-owned Kooima views (#396 W7)
#include <openxr/XR_EXT_display_zones.h> // bottom-75% tiger zone framing (ADR-027, #568)
// XrCompositionLayerWindowSpaceEXT — the shared window-space layer struct is
// declared (ifndef-guarded) in the window-binding headers; the cocoa one is
// plain C with no platform deps, so it serves as the decl source on Android.
#include <openxr/XR_EXT_cocoa_window_binding.h>

#include <atomic>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <jni.h>
#include <mutex>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>

#include "hud_bar.h"
#include "model_renderer.h"
#include "model_vulkan_utils.h"  // scratch image/buffer for the click-through silhouette

#define LOG_TAG "model_viewer_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)      __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...) do {                                                                  \
		static bool _logged = false;                                                                \
		if (!_logged) { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP[once]: " __VA_ARGS__); _logged = true; } \
	} while (0)
#else
#define DXR_HW_DBG(...)      ((void)0)
#define DXR_HW_DBG_ONCE(...) ((void)0)
#endif

namespace {

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS:                       return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:   return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
	default:                               return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name != nullptr) {
		LOGI("%s -> %s", what, name);
	} else {
		LOGI("%s -> XrResult(%d)", what, (int)r);
	}
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);

VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;

XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;

// Overlay mode (#558): the runtime weaves the tiger into a service-owned
// TYPE_APPLICATION_OVERLAY above the LIVE launcher. The avatar then relinquishes
// the foreground (MainActivity.moveTaskToBack) so the launcher resumes + stays
// interactive, and a keep-alive foreground service keeps this process off the
// Android freezer. In that state the NativeActivity has no window, but in OOP we
// render to the IPC swapchain (not our own window) — so keep rendering anyway.
// Gated on `setprop debug.dxr.overlay 1`. Read once at android_main start.
bool g_overlay_mode = false;

// ── Display rendering-mode switching (XR_EXT_display_info) ─────────────────
// The runtime advertises a set of display rendering modes (e.g. 3D-stereo,
// 2D-mono); `setprop debug.dxr.mode N` selects one. Mode requests are async —
// the runtime applies them and (if view count changes) the next frame adapts.
PFN_xrEnumerateDisplayRenderingModesEXT g_pfnEnumModes = nullptr;
PFN_xrRequestDisplayRenderingModeEXT g_pfnReqMode = nullptr;
uint32_t g_rmode_count = 0;
std::atomic<uint32_t> g_rmode_current{0};
bool g_rmode_requestable = false;
char g_rmode_names[8][64] = {};  // mode names (debug.dxr.mode hook / logs)
// Per-mode tiling (multiview-tiling invariant): ONE atlas swapchain created once
// at init, sized worst-case over all modes × both orientations. Per frame the app
// renders the ACTIVE mode's tiles (display × view_scale) into the atlas at the
// mode's tile-layout offsets, and submits each projection view's subImage.imageRect
// = its tile rect (the compositor maps eye→destination tile by index). 2D = 1 tile
// (1x1, scale 1.0), 3D = 2 tiles (2x1, scale 0.75). Indexed by modeIndex.
float g_rmode_scale_x[8] = {};
float g_rmode_scale_y[8] = {};
uint32_t g_rmode_vc[8] = {};   // view count per mode (2D=1, LeiaSR=2)
uint32_t g_rmode_cols[8] = {}; // tile columns (2D=1, LeiaSR=2)
uint32_t g_rmode_rows[8] = {}; // tile rows (1)
// Worst-case ATLAS dims = max over modes × orientations of (cols×tile, rows×tile)
// where tile = max(viewWidthPixels, viewHeightPixels). For a 2560×1600 panel:
// 3D 2x1 → 3840×1920, 2D 1x1 → 2560×2560 ⇒ atlas 3840×2560. The single atlas
// swapchain is sized to this and never reallocated on a mode switch / rotation.
uint32_t g_atlas_w = 0;
uint32_t g_atlas_h = 0;

// #568 speech bubble — a rounded panel + greeting, rasterized like the button
// bar but submitted as an XR_EXT_local_3d_zone Local2D layer in the top-25% 2D
// band (above the tiger zone). Reuses the HudBar swapchain/font/upload infra.
HudBar g_bubble;
constexpr uint32_t kBubbleTexW = 1024;
constexpr uint32_t kBubbleTexH = 384;
static const char *const kBubbleText = "Hi there! I'm Leo, your friendly 3D desktop avatar.";
// Live window pixel size (from the rig raw channel) — drives canvas sizing and
// the bottom-75% zone framing each frame.
std::atomic<uint32_t> g_win_px_w{0};
std::atomic<uint32_t> g_win_px_h{0};

// ── Per-pixel gesture/click-through silhouette (windows/main.cpp UpdateSilhouette)
// Each silhouette tick the renderer draws the tiger (UNION over the rendered
// views) into a small scratch image with transparent bg → alpha marks the
// avatar. Published as a coverage bitmap the touch handler samples to gate
// gestures to the tiger area (and, next, for click-through). Maps to the ZONE
// rect (bottom-75%) so the scratch is fully covered (no stale top band); the
// touch handler rebases canvas→zone before sampling.
struct SilhouetteCoverage
{
	std::mutex mtx;
	std::vector<uint8_t> bits;                        // covW*covH, 1 = tiger present
	int covW = 0, covH = 0;                           // coverage bitmap dims
	int zoneX = 0, zoneY = 0, zoneW = 0, zoneH = 0;   // canvas-px rect the bits map to
	bool ready = false;
};
SilhouetteCoverage g_sil;
ModelImage g_sil_image = {};
ModelBuffer g_sil_readback = {};
void *g_sil_mapped = nullptr;
VkCommandPool g_sil_pool = VK_NULL_HANDLE;

// True if a canvas-space point lands on the tiger silhouette (dilated). Called
// from nativeOnTouch (UI thread). Fail-OPEN until the first coverage is ready so
// gestures still work during warmup.
bool
sil_hit(float cx, float cy)
{
	std::lock_guard<std::mutex> lock(g_sil.mtx);
	if (!g_sil.ready || g_sil.covW <= 0 || g_sil.covH <= 0 || g_sil.zoneW <= 0 ||
	    g_sil.zoneH <= 0)
		return true;  // not measured yet → don't block input
	const float zx = cx - (float)g_sil.zoneX;
	const float zy = cy - (float)g_sil.zoneY;
	if (zx < 0.0f || zy < 0.0f || zx >= (float)g_sil.zoneW || zy >= (float)g_sil.zoneH)
		return false;  // outside the tiger zone (e.g. the top-25% bubble band)
	const int sx = (int)(zx * (float)g_sil.covW / (float)g_sil.zoneW);
	const int sy = (int)(zy * (float)g_sil.covH / (float)g_sil.zoneH);
	const int R = 2;  // dilation radius in coverage px — forgiving near edges
	for (int dy = -R; dy <= R; ++dy) {
		for (int dx = -R; dx <= R; ++dx) {
			const int x = sx + dx, y = sy + dy;
			if (x < 0 || y < 0 || x >= g_sil.covW || y >= g_sil.covH) continue;
			if (g_sil.bits[(size_t)y * g_sil.covW + x]) return true;
		}
	}
	return false;
}

// ── XR_EXT_view_rig (#396 W7) ───────────────────────────────────────────────
// When the runtime advertises XR_EXT_view_rig, chain an XrDisplayRigEXT on
// xrLocateViews and consume the render-ready off-axis XrView{pose,fov} the
// runtime computes (server-side Kooima over IPC on Android, #510/#513). The
// rig pose stays IDENTITY: this app orbits the MODEL (build_splat_model), not
// the camera, so the virtual display plane sits at the world origin and the
// model frames AT screen depth (no forward push — see load_model_index).
bool g_has_view_rig = false;

// ── XR_EXT_display_zones (ADR-027, #568) — bottom-75% tiger zone ────────────
// The avatar (like the Windows leg) confines its 3D content to the bottom 75%
// of the canvas (the top 25% is reserved for the speech bubble). With the zone
// chained on BOTH the zone-scoped locate (→ off-axis FOV framed to the rect)
// AND the submitted projection layer (→ runtime composites the view tile into
// the rect), the runtime owns the framing — no app-side Kooima.
bool g_has_display_zones = false;
bool g_has_local_3d_zone = false;   // XR_EXT_display_zones requires it (>= v4)
PFN_xrGetDisplayZoneCapabilitiesEXT g_pfnGetDisplayZoneCaps = nullptr;
PFN_xrGetDisplayZoneRecommendedViewSizeEXT g_pfnGetDisplayZoneViewSize = nullptr;
bool g_zones_active = false;          // caps query passed; zone framing live
// Top fraction reserved for the (future) speech bubble; tiger gets the rest.
constexpr float kBubbleBandFrac = 0.25f;
float g_rig_vh = 1.33f;  // virtual display height (app units); refit per model

// Worst-case view/tile capacity for the one atlas swapchain (multiview-tiling
// invariant): the array sizing must cover the runtime's MAX advertised view
// count — Leia maxes at 2 (LeiaSR stereo) but sim_display advertises a 4-view
// "Quad" mode, so size for 4. The render loop only ever renders/submits the
// ACTIVE mode's view count (g_rmode_vc), never all kViewCount. (#568)
constexpr uint32_t kViewCount = 4;

struct PerView
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	XrSwapchainImageVulkanKHR images[8]{};
	uint32_t image_count{0};
};
PerView g_views[kViewCount];

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;

// The 3DGS compute renderer (8 compute shaders). Renders sample.glb to
// each eye's swapchain image; the runtime's Leia DP weaves them.
ModelRenderer g_model;
bool g_model_ready = false;
std::atomic<bool> g_scene_loaded{false};

// Bundled models (shipped in src/main/assets/, byte-copied out of the APK at
// load). v1 ships the demo's avatar: assets/tiger/avatar.fbx (the same asset
// the Windows leg auto-loads), parsed by the ufbx FBX backend. Its rgb.jpg
// texture is staged alongside (see load_model_index).
const char *const kModels[] = {
    "avatar.fbx",
};
// Sidecar files staged next to each model (FBX references its texture by a
// relative path, like the Windows demo which copies rgb.jpg next to the exe).
const char *const kModelSidecars[] = {
    "rgb.jpg",
};
constexpr int kSidecarCount = (int)(sizeof(kModelSidecars) / sizeof(kModelSidecars[0]));
constexpr int kModelCount = (int)(sizeof(kModels) / sizeof(kModels[0]));
std::atomic<int> g_model_index{0};    // which model is loaded
// Double-tap recenters the camera (pan/dolly/zoom back to the framed default);
// serviced on the android_main thread (no Vulkan work, no model reload).
std::atomic<bool> g_reset_view_request{false};

// Scene framing: the .spz is centered near the world origin, which is also
// where the head/reference space sits — so by default the splat is right on
// top of the camera (huge). Recenter it and push it g_scene_push metres in
// front so it frames at a comfortable size. Pure translation => gaussian
// sizes stay correct (true perspective zoom, not a blob rescale). The push
// distance is auto-derived from the scene extent at load (bigger =
// smaller/further); tune the multiplier in load_butterfly if needed.
float g_scene_center[3] = {0.0f, 0.0f, 0.0f};
std::atomic<float> g_scene_push{1.5f};
// glTF models come in arbitrary units (the Fox is ~155 units long); normalize
// the model to ~kTargetSize metres so it fits the display + the 0.01–100 clip.
std::atomic<float> g_scene_scale{1.0f};
float g_fit_scale = 1.0f;             // auto-fit scale; pinch-zoom multiplies around it
constexpr float kTargetSize = 0.95f;  // fill the view like the Windows auto-fit (model.height*1.4)
// Flip the splat vertically (some splats are trained Y-up vs Y-down).
bool g_flip_y = false;  // glTF is Y-up already (the butterfly point cloud was upside-down)
// Touch-drag camera controls (Windows parity): the avatar never rotates. Vertical
// drag = depth dolly (Windows W/S → cameraPosZ); horizontal drag = lateral strafe
// (Windows A/D → cameraPosX). Both offset the virtual-display rig pose, baselined
// on the animated anchor; the avatar's HEADING is owned by the face-the-viewer
// billboard (see the render loop), exactly like windows/main.cpp.
std::atomic<float> g_cam_pan_x{0.0f};    // horizontal drag → lateral strafe
std::atomic<float> g_cam_dolly_z{0.0f};  // vertical drag   → depth dolly

std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};
uint64_t g_frame_count = 0;

// ─── matrix helpers (column-major float[16]) ─────────────────────────────
struct Mat4 { float m[16]; };

Mat4
view_matrix_from_pose(const XrPosef &pose)
{
	const float x = pose.orientation.x, y = pose.orientation.y;
	const float z = pose.orientation.z, w = pose.orientation.w;
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;

	const float r00 = 1.0f - 2.0f * (yy + zz);
	const float r01 = 2.0f * (xy + wz);
	const float r02 = 2.0f * (xz - wy);
	const float r10 = 2.0f * (xy - wz);
	const float r11 = 1.0f - 2.0f * (xx + zz);
	const float r12 = 2.0f * (yz + wx);
	const float r20 = 2.0f * (xz + wy);
	const float r21 = 2.0f * (yz - wx);
	const float r22 = 1.0f - 2.0f * (xx + yy);

	const float tx = -(r00 * pose.position.x + r01 * pose.position.y + r02 * pose.position.z);
	const float ty = -(r10 * pose.position.x + r11 * pose.position.y + r12 * pose.position.z);
	const float tz = -(r20 * pose.position.x + r21 * pose.position.y + r22 * pose.position.z);

	Mat4 v{};
	v.m[0]  = r00; v.m[1]  = r10; v.m[2]  = r20; v.m[3]  = 0.0f;
	v.m[4]  = r01; v.m[5]  = r11; v.m[6]  = r21; v.m[7]  = 0.0f;
	v.m[8]  = r02; v.m[9]  = r12; v.m[10] = r22; v.m[11] = 0.0f;
	v.m[12] = tx;  v.m[13] = ty;  v.m[14] = tz;  v.m[15] = 1.0f;
	return v;
}

// Display-local eye distance for the ZDP-anchored clip: z of (rigPose^-1 *
// eyeWorld). Degenerates to pose.position.z at identity rig pose. Mirrors
// rig_local_eye_z in cube_handle_vk_android / RigLocalEyeZ in the desktop legs
// (per-platform helper duplication is the accepted W7 pattern).
float
rig_local_eye_z(const XrPosef &rig, const XrVector3f &eye_world)
{
	const float dx = eye_world.x - rig.position.x;
	const float dy = eye_world.y - rig.position.y;
	const float dz = eye_world.z - rig.position.z;
	const float qx = -rig.orientation.x, qy = -rig.orientation.y;
	const float qz = -rig.orientation.z, qw = rig.orientation.w;
	const float cx = qy * dz - qz * dy + qw * dx;
	const float cy = qz * dx - qx * dz + qw * dy;
	return dz + 2.0f * (qx * cy - qy * cx);
}

// Asymmetric perspective from OpenXR FOV, direct [0,1] clip depth. NO Y
// negation here: ModelRenderer (W7, #396) consumes a clean +Y-up view +
// off-axis projection and flips Vulkan Y at the RASTER stage via a
// negative-height viewport in renderEye. aspect_w_over_h > 0 forces a
// SYMMETRIC frustum (legacy no-rig fallback only) — under XR_EXT_view_rig
// pass -1: the rig FOV is render-ready off-axis and must pass through.
Mat4
projection_matrix_from_fov(const XrFovf &fov, float aspect_w_over_h, float near_z, float far_z)
{
	const float tan_l = std::tan(fov.angleLeft);
	const float tan_r = std::tan(fov.angleRight);
	const float tan_d = std::tan(fov.angleDown);
	const float tan_u = std::tan(fov.angleUp);
	const float tan_w = tan_r - tan_l;
	const float tan_h = tan_u - tan_d;

	Mat4 p{};
	p.m[0]  = 2.0f / tan_w;
	p.m[5]  = 2.0f / tan_h;
	p.m[8]  = (tan_r + tan_l) / tan_w;
	p.m[9]  = (tan_u + tan_d) / tan_h;
	p.m[10] = -far_z / (far_z - near_z);
	p.m[11] = -1.0f;
	p.m[14] = -(far_z * near_z) / (far_z - near_z);

	if (aspect_w_over_h > 0.0f) {
		p.m[0] = (2.0f / tan_h) / aspect_w_over_h;
		p.m[8] = 0.0f;
	}
	return p;
}

// #568: derive the zone sub-frustum from the full-canvas off-axis FOV. The
// Android OOP runtime returns a FULL-CANVAS view_rig FOV — the zone-scoped
// locate's canvas rebase (ipc_server_handler u_canvas_apply_to_metrics) is gated
// on Windows-only D3D11 window metrics, so it no-ops on Android — so the app
// reframes the Kooima to the zone rect itself: take the linear sub-range of the
// tangent space the zone rect covers. Off-axis (stereo) skew is preserved (a
// linear sub-range of tangents). canvas px (cw,ch) is y-down; FOV angleUp=+Y so
// canvas TOP (y=0)→angleUp, BOTTOM (y=ch)→angleDown.
static XrFovf
zone_subfrustum_fov(const XrFovf &full, uint32_t cw, uint32_t ch,
                    int32_t zx, int32_t zy, int32_t zw, int32_t zh)
{
	if (cw == 0 || ch == 0) {
		return full;
	}
	const float tl = std::tan(full.angleLeft), tr = std::tan(full.angleRight);
	const float td = std::tan(full.angleDown), tu = std::tan(full.angleUp);
	const float fx0 = (float)zx / (float)cw;
	const float fx1 = (float)(zx + zw) / (float)cw;
	const float fy0 = (float)zy / (float)ch;        // zone top fraction
	const float fy1 = (float)(zy + zh) / (float)ch; // zone bottom fraction
	XrFovf out;
	out.angleLeft = std::atan(tl + fx0 * (tr - tl));
	out.angleRight = std::atan(tl + fx1 * (tr - tl));
	out.angleUp = std::atan(tu + fy0 * (td - tu));
	out.angleDown = std::atan(tu + fy1 * (td - tu));
	return out;
}

Mat4
mat4_identity()
{
	Mat4 r{};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

// Column-major a*b.
Mat4
mat4_mul(const Mat4 &a, const Mat4 &b)
{
	Mat4 r{};
	for (int c = 0; c < 4; ++c) {
		for (int row = 0; row < 4; ++row) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k) {
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			}
			r.m[c * 4 + row] = s;
		}
	}
	return r;
}

// Splat model transform (same for both eyes): recenter on the scene centroid,
// optional Y-flip, yaw about Y (the face-the-viewer billboard heading — NOT drag;
// the avatar stays upright, no pitch), then push g_scene_push m in front.
// M = place * Translate(0,0,-push) * RotY(angle) * ScaleY(flip?-1:1) * Translate(-center)
Mat4
build_splat_model(float angle)
{
	Mat4 recenter = mat4_identity();
	recenter.m[12] = -g_scene_center[0];
	recenter.m[13] = -g_scene_center[1];
	recenter.m[14] = -g_scene_center[2];

	Mat4 flip = mat4_identity();
	if (g_flip_y) {
		flip.m[5] = -1.0f;
	}

	const float c = std::cos(angle), s = std::sin(angle);
	Mat4 roty = mat4_identity();
	roty.m[0] = c;  roty.m[2] = -s;
	roty.m[8] = s;  roty.m[10] = c;

	const float sc = g_scene_scale.load(std::memory_order_relaxed);
	Mat4 scale = mat4_identity();
	scale.m[0] = sc; scale.m[5] = sc; scale.m[10] = sc;

	Mat4 push = mat4_identity();
	push.m[14] = -g_scene_push.load(std::memory_order_relaxed);

	// #570: match macOS/Windows — render the model IN PLACE at the scene centre
	// (the rig pose is set to the same centre), rather than recentring to the
	// world origin under an identity rig. Spin/scale happen about the centre,
	// then `place` puts the model back at the centre so model and rig coincide.
	Mat4 place = mat4_identity();
	place.m[12] = g_scene_center[0];
	place.m[13] = g_scene_center[1];
	place.m[14] = g_scene_center[2];

	// place * push * roty * flip * scale * recenter (yaw/scale about the centre,
	// model rendered in place at the centre — rig coincides with it). Upright: no
	// pitch (the billboard owns heading; tilting the avatar looks wrong).
	return mat4_mul(place,
	    mat4_mul(push, mat4_mul(roty, mat4_mul(flip, mat4_mul(scale, recenter)))));
}

// Lazily (re)create the silhouette scratch image + host-visible readback buffer
// + a transfer command pool. Sized to the downscaled coverage; resized on demand.
bool
ensure_sil_targets(uint32_t w, uint32_t h)
{
	if (g_sil_image.image != VK_NULL_HANDLE && g_sil_image.width == w &&
	    g_sil_image.height == h)
		return true;
	if (g_sil_image.image != VK_NULL_HANDLE) modelDestroyImage(g_vk_device, g_sil_image);
	if (g_sil_readback.buffer != VK_NULL_HANDLE) {
		if (g_sil_mapped) {
			vkUnmapMemory(g_vk_device, g_sil_readback.memory);
			g_sil_mapped = nullptr;
		}
		modelDestroyBuffer(g_vk_device, g_sil_readback);
	}
	g_sil_image = modelCreateImage2D(g_vk_device, g_vk_phys_device, w, h,
	    VK_FORMAT_R8G8B8A8_UNORM,
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (g_sil_image.image == VK_NULL_HANDLE) return false;
	g_sil_readback = modelCreateBuffer(g_vk_device, g_vk_phys_device, (VkDeviceSize)w * h * 4,
	    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (g_sil_readback.buffer == VK_NULL_HANDLE) return false;
	if (vkMapMemory(g_vk_device, g_sil_readback.memory, 0, (VkDeviceSize)w * h * 4, 0,
	                &g_sil_mapped) != VK_SUCCESS)
		return false;
	if (g_sil_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = g_vk_queue_family;
		if (vkCreateCommandPool(g_vk_device, &pci, nullptr, &g_sil_pool) != VK_SUCCESS)
			return false;
	}
	return g_sil_mapped != nullptr;
}

// Render the tiger (UNION over the rendered views — first+last, like the Windows
// leg) into the scratch image, read alpha back, publish the coverage bitmap.
// windows/main.cpp UpdateSilhouette, adapted: the scratch maps to the ZONE rect
// (bottom-75%), so renderEye fills the WHOLE scratch (no stale top band). imgW/
// imgH MUST match the atlas dims so the renderer's internal target isn't
// reallocated (same caveat as the main eye render). (#568)
void
update_silhouette(uint32_t imgW, uint32_t imgH, int zoneX, int zoneY, int zoneW, int zoneH,
                  const Mat4 *evMs, const Mat4 *projMs, const float *clipFars, uint32_t numViews)
{
	if (zoneW <= 0 || zoneH <= 0 || numViews == 0) return;
	uint32_t w = (uint32_t)zoneW / 4;
	if (w < 64) w = 64; else if (w > 512) w = 512;
	uint32_t h = (uint32_t)zoneH / 4;
	if (h < 64) h = 64; else if (h > 512) h = 512;
	if (!ensure_sil_targets(w, h)) return;

	const uint32_t idx[2] = {0, numViews - 1};  // outermost views bound the disparity spread
	const uint32_t passes = (numViews > 1) ? 2u : 1u;
	std::vector<uint8_t> uni((size_t)w * h, 0);

	for (uint32_t p = 0; p < passes; ++p) {
		const uint32_t v = idx[p];
		// Draw the tiger into the whole scratch (zone-framed projM → 1:1 with the
		// zone rect). Transparent bg → alpha is the silhouette.
		g_model.renderEye(g_sil_image.image, VK_FORMAT_R8G8B8A8_UNORM, imgW, imgH,
		    0, 0, w, h, evMs[v].m, projMs[v].m, /*transparentBg*/ true, clipFars[v]);
		// renderEye leaves the scratch in COLOR_ATTACHMENT_OPTIMAL → copy to host.
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = g_sil_pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(g_vk_device, &ai, &cmd) != VK_SUCCESS) return;
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
		VkImageMemoryBarrier to_src = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_src.image = g_sil_image.image;
		to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
		VkBufferImageCopy region = {};
		region.bufferRowLength = w;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		vkCmdCopyImageToBuffer(cmd, g_sil_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    g_sil_readback.buffer, 1, &region);
		vkEndCommandBuffer(cmd);
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);
		vkQueueWaitIdle(g_vk_queue);
		vkFreeCommandBuffers(g_vk_device, g_sil_pool, 1, &cmd);

		const uint8_t *px = (const uint8_t *)g_sil_mapped;
		if (!px) return;
		for (uint32_t i = 0; i < w * h; ++i) {
			const uint8_t a = px[i * 4 + 3];
			if (a > uni[i]) uni[i] = a;
		}
	}

	uint32_t covered = 0;
	for (uint32_t i = 0; i < w * h; ++i)
		if (uni[i] > 40) ++covered;
	static bool s_sil_logged = false;
	if (!s_sil_logged && covered > 0) {
		LOGI("silhouette: cov %ux%u zone=(%d,%d %dx%d) covered=%u%% (gesture gate live)", w, h,
		     zoneX, zoneY, zoneW, zoneH, covered * 100u / (w * h));
		s_sil_logged = true;
	}
	std::lock_guard<std::mutex> lock(g_sil.mtx);
	g_sil.bits.resize((size_t)w * h);
	for (uint32_t i = 0; i < w * h; ++i) g_sil.bits[i] = (uni[i] > 40) ? 1 : 0;
	g_sil.covW = (int)w;
	g_sil.covH = (int)h;
	g_sil.zoneX = zoneX;
	g_sil.zoneY = zoneY;
	g_sil.zoneW = zoneW;
	g_sil.zoneH = zoneH;
	g_sil.ready = true;
}

// ─── OpenXR-Android bring-up (reused verbatim from cube_handle_vk_android) ─
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
	if (res != XR_SUCCESS || xrInitializeLoaderKHR == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
		return false;
	}
	XrLoaderInitInfoAndroidKHR loader_init = {};
	loader_init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	loader_init.applicationVM = app->activity->vm;
	loader_init.applicationContext = app->activity->clazz;
	res = xrInitializeLoaderKHR(
	    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init));
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	g_runtime_unavailable.store(false, std::memory_order_relaxed);

	// XR_EXT_view_rig is enabled only when advertised (#396 W7); without it
	// the app keeps the legacy raw-locate + fixed-clip path.
	g_has_view_rig = false;
	{
		uint32_t n = 0;
		if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) == XR_SUCCESS && n > 0) {
			std::vector<XrExtensionProperties> props(n);
			for (auto &p : props) {
				p.type = XR_TYPE_EXTENSION_PROPERTIES;
				p.next = nullptr;
			}
			if (xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()) == XR_SUCCESS) {
				for (uint32_t i = 0; i < n; ++i) {
					if (std::strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0) {
						g_has_view_rig = true;
					}
					if (std::strcmp(props[i].extensionName, XR_EXT_DISPLAY_ZONES_EXTENSION_NAME) == 0) {
						g_has_display_zones = true;
					}
					if (std::strcmp(props[i].extensionName, XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME) == 0) {
						g_has_local_3d_zone = true;
					}
				}
			}
		}
		LOGI("XR_EXT_view_rig advertised: %s; XR_EXT_display_zones: %s",
		     g_has_view_rig ? "yes" : "no", g_has_display_zones ? "yes" : "no");
	}

	const char *extensions[6] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	    XR_EXT_DISPLAY_INFO_EXTENSION_NAME,  // display rendering-mode switching
	};
	uint32_t extension_count = 3;
	if (g_has_view_rig) {
		extensions[extension_count++] = XR_EXT_VIEW_RIG_EXTENSION_NAME;
	}
	// XR_EXT_display_zones requires XR_EXT_local_3d_zone (>=v4) + XR_EXT_view_rig
	// (>=v2) — enable all three together or none (the runtime rejects the
	// instance with XR_ERROR_VALIDATION_FAILURE otherwise).
	if (g_has_display_zones && g_has_view_rig && g_has_local_3d_zone) {
		extensions[extension_count++] = XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME;
		extensions[extension_count++] = XR_EXT_DISPLAY_ZONES_EXTENSION_NAME;
	} else {
		g_has_display_zones = false;
	}
	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "model_viewer_vk_android", XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount = extension_count;
	create_info.enabledExtensionNames = extensions;

	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&create_info, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) {
			break;
		}
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5); launch the "
		     "DisplayXR app once if this persists…", attempt + 1);
		usleep(400 * 1000);
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE) {
			g_runtime_unavailable.store(true, std::memory_order_relaxed);
		}
		return false;
	}
	LOGI("ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS");
	return true;
}

bool
query_system_and_graphics_reqs()
{
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &sys_info, &g_system_id);
	log_xr_result("xrGetSystem(HMD)", res);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &sys_info, &g_system_id);
		log_xr_result("xrGetSystem(HANDHELD)", res);
		if (res != XR_SUCCESS) {
			return false;
		}
	}

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	if (res != XR_SUCCESS || get_reqs == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsRequirements2) failed (%d)", (int)res);
		return false;
	}
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) {
		return false;
	}
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR xr_create_vk_instance = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanInstanceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_instance));
	if (res != XR_SUCCESS || xr_create_vk_instance == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanInstance) failed (%d)", (int)res);
		return false;
	}
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "model_viewer_vk_android";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "displayxr";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_MAKE_VERSION(
	    XR_VERSION_MAJOR(g_required_vk_version),
	    XR_VERSION_MINOR(g_required_vk_version), 0);

	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;

	XrVulkanInstanceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanCreateInfo = &vk_ci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_instance(g_instance, &xr_ci, &g_vk_instance, &vk_result);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	return true;
}

bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR xr_get_phys = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsDevice2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_get_phys));
	if (res != XR_SUCCESS || xr_get_phys == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsDevice2) failed (%d)", (int)res);
		return false;
	}
	XrVulkanGraphicsDeviceGetInfoKHR info = {};
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = g_system_id;
	info.vulkanInstance = g_vk_instance;
	res = xr_get_phys(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

bool
create_vulkan_device()
{
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, nullptr);
	if (qf_count == 0) {
		LOGE("No Vulkan queue families");
		return false;
	}
	VkQueueFamilyProperties qf_props[16] = {};
	const uint32_t qf_cap = sizeof(qf_props) / sizeof(qf_props[0]);
	if (qf_count > qf_cap) {
		qf_count = qf_cap;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, qf_props);

	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; ++i) {
		if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_vk_queue_family = i;
			break;
		}
	}
	if (g_vk_queue_family == UINT32_MAX) {
		LOGE("No graphics-capable queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	// The 3DGS radix-sort compute shaders (sort/hist/preprocess_sort) use
	// uint64_t keys via GL_EXT_shader_explicit_arithmetic_types_int64, so the
	// VkDevice MUST enable shaderInt64 or those pipelines fail to LINK on
	// Adreno (-> null pipeline -> renderEye SIGSEGV). The runtime's
	// xrCreateVulkanDeviceKHR copies our pEnabledFeatures through verbatim
	// (oxr_vk_create_vulkan_device), so requesting it here is enough. Query
	// the BASE 1.0 features (not vkGetPhysicalDeviceFeatures2 — that returns
	// zeros in this 1.0-instance context and falsely reported shaderInt64
	// absent); the base query always carries shaderInt64.
	VkPhysicalDeviceFeatures supported = {};
	vkGetPhysicalDeviceFeatures(g_vk_phys_device, &supported);
	LOGI("device shaderInt64=%d", (int)supported.shaderInt64);

	VkPhysicalDeviceFeatures features = {};
	features.shaderInt64 = supported.shaderInt64;

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &features;

	PFN_xrCreateVulkanDeviceKHR xr_create_vk_device = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanDeviceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_device));
	if (res != XR_SUCCESS || xr_create_vk_device == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanDevice) failed (%d)", (int)res);
		return false;
	}
	XrVulkanDeviceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanPhysicalDevice = g_vk_phys_device;
	xr_ci.vulkanCreateInfo = &dci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_device(g_instance, &xr_ci, &g_vk_device, &vk_result);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
	LOGI("Vulkan device ready: queue_family=%u", g_vk_queue_family);
	return true;
}

bool
create_session()
{
	XrGraphicsBindingVulkanKHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding.instance = g_vk_instance;
	binding.physicalDevice = g_vk_phys_device;
	binding.device = g_vk_device;
	binding.queueFamilyIndex = g_vk_queue_family;
	binding.queueIndex = 0;

	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_system_id;
	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);
	if (res != XR_SUCCESS) {
		return false;
	}

	// ── Display-zones bring-up (ADR-027, #568): resolve the entry points and
	// query caps. On success the render loop chains a bottom-75% tiger zone so
	// the runtime frames the avatar into the lower band (top 25% = bubble).
	if (g_has_display_zones) {
		xrGetInstanceProcAddr(g_instance, "xrGetDisplayZoneCapabilitiesEXT",
		                      (PFN_xrVoidFunction *)&g_pfnGetDisplayZoneCaps);
		xrGetInstanceProcAddr(g_instance, "xrGetDisplayZoneRecommendedViewSizeEXT",
		                      (PFN_xrVoidFunction *)&g_pfnGetDisplayZoneViewSize);
		if (g_pfnGetDisplayZoneCaps && g_pfnGetDisplayZoneViewSize) {
			XrDisplayZoneCapabilitiesEXT caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT};
			XrResult cr = g_pfnGetDisplayZoneCaps(g_session, &caps);
			g_zones_active = (cr == XR_SUCCESS && caps.supported && caps.maxZones3D >= 1);
			LOGI("Display zones: caps result=0x%x supported=%d maxZones3D=%u -> active=%d",
			     (unsigned)cr, (int)caps.supported, caps.maxZones3D, (int)g_zones_active);
		} else {
			LOGW("Display zones: entry points unresolved; disabling");
			g_has_display_zones = false;
		}
	}
	return true;
}

// Resolve the rendering-mode entry points and enumerate what the runtime/DP
// advertises. Best-effort: logs each mode so we can see (on this device) whether
// modes differ in view count/tiling (→ render-loop work) or are a pure DP-side
// 2D/3D toggle (→ app just requests them). Never fails bring-up.
bool
enumerate_rendering_modes()
{
	xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRenderingModesEXT",
	                      (PFN_xrVoidFunction *)&g_pfnEnumModes);
	xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRenderingModeEXT",
	                      (PFN_xrVoidFunction *)&g_pfnReqMode);
	if (g_pfnEnumModes == nullptr || g_pfnReqMode == nullptr) {
		LOGI("Display rendering-mode ext entry points not resolved — mode switch disabled");
		return true;
	}
	uint32_t count = 0;
	XrResult res = g_pfnEnumModes(g_session, 0, &count, nullptr);
	if (res != XR_SUCCESS || count == 0) {
		LOGI("xrEnumerateDisplayRenderingModesEXT: count=%u res=%d", count, (int)res);
		return true;
	}
	std::vector<XrDisplayRenderingModeInfoEXT> modes(count);
	for (auto &m : modes) {
		m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		m.next = nullptr;
	}
	res = g_pfnEnumModes(g_session, count, &count, modes.data());
	if (res != XR_SUCCESS) {
		LOGI("xrEnumerateDisplayRenderingModesEXT (fill) res=%d", (int)res);
		return true;
	}
	g_rmode_count = count;
	for (uint32_t i = 0; i < count; ++i) {
		const auto &m = modes[i];
		if (m.modeIndex < 8) {
			std::strncpy(g_rmode_names[m.modeIndex], m.modeName,
			             sizeof(g_rmode_names[0]) - 1);
			g_rmode_scale_x[m.modeIndex] = m.viewScaleX > 0.0f ? m.viewScaleX : 1.0f;
			g_rmode_scale_y[m.modeIndex] = m.viewScaleY > 0.0f ? m.viewScaleY : 1.0f;
			g_rmode_vc[m.modeIndex] = m.viewCount > 0 ? m.viewCount : 1;
			const uint32_t cols = m.tileColumns > 0 ? m.tileColumns : 1;
			const uint32_t rows = m.tileRows > 0 ? m.tileRows : 1;
			g_rmode_cols[m.modeIndex] = cols;
			g_rmode_rows[m.modeIndex] = rows;
			// Worst-case ATLAS over modes × orientations: a tile is at most
			// max(W,H) on a side in either orientation, so cols×edge × rows×edge
			// bounds this mode's atlas; take the per-axis max across modes.
			const uint32_t edge =
			    m.viewWidthPixels > m.viewHeightPixels ? m.viewWidthPixels : m.viewHeightPixels;
			if (cols * edge > g_atlas_w) g_atlas_w = cols * edge;
			if (rows * edge > g_atlas_h) g_atlas_h = rows * edge;
		}
		if (m.isActive) {
			g_rmode_current.store(m.modeIndex, std::memory_order_relaxed);
			g_rmode_requestable = m.isRequestable;
		}
		LOGI("RMODE[%u] idx=%u \"%s\" views=%u scale=(%.3f,%.3f) hw3D=%d tiles=%ux%u "
		     "%ux%u active=%d requestable=%d",
		     i, m.modeIndex, m.modeName, m.viewCount, m.viewScaleX, m.viewScaleY,
		     (int)m.hardwareDisplay3D, m.tileColumns, m.tileRows, m.viewWidthPixels,
		     m.viewHeightPixels, (int)m.isActive, (int)m.isRequestable);
	}
	LOGI("Display rendering modes: %u total, current=%u, requestable=%d",
	     g_rmode_count, g_rmode_current.load(std::memory_order_relaxed),
	     (int)g_rmode_requestable);
	return true;
}

bool
create_swapchains()
{
	uint32_t expected_view_count = 0;
	XrResult res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    0, &expected_view_count, nullptr);
	// Accept any view count the runtime advertises, up to our worst-case
	// capacity (kViewCount). The active rendering mode picks the actual tile
	// count at submit time; here we only need the atlas sized for the max.
	if (res != XR_SUCCESS || expected_view_count < 1 || expected_view_count > kViewCount) {
		LOGE("Runtime view count %u out of range [1,%u]", expected_view_count, kViewCount);
		return false;
	}
	XrViewConfigurationView view_configs[kViewCount] = {};
	for (uint32_t i = 0; i < kViewCount; ++i) {
		view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    kViewCount, &expected_view_count, view_configs);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateViewConfigurationViews", res);
		return false;
	}

	uint32_t format_count = 0;
	res = xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr);
	if (res != XR_SUCCESS || format_count == 0) {
		log_xr_result("xrEnumerateSwapchainFormats(count)", res);
		return false;
	}
	int64_t formats[64] = {};
	if (format_count > 64) {
		format_count = 64;
	}
	res = xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainFormats(fill)", res);
		return false;
	}
	const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
	for (int64_t pref : preferred) {
		for (uint32_t i = 0; i < format_count && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i) {
			if (formats[i] == pref) {
				g_swapchain_format = (VkFormat)pref;
			}
		}
		if (g_swapchain_format != VK_FORMAT_UNDEFINED) {
			break;
		}
	}
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) {
		g_swapchain_format = (VkFormat)formats[0];
	}
	LOGI("Chose swapchain format: 0x%x", (uint32_t)g_swapchain_format);

	// ONE atlas swapchain, sized worst-case over all modes × orientations
	// (multiview-tiling invariant). Allocated once here and never reallocated on a
	// mode switch / rotation; each frame the app renders the active mode's tiles
	// into sub-rects of this atlas (active_tile_dims + the tile-layout offsets) and
	// submits each projection view's subImage.imageRect = its tile rect. All views
	// reference this single swapchain (g_views[0]); g_views[1] aliases it.
	uint32_t aw = g_atlas_w, ah = g_atlas_h;
	if (aw == 0 || ah == 0) { // no mode info → 2x1 of the recommended rect
		aw = view_configs[0].recommendedImageRectWidth * 2;
		ah = view_configs[0].recommendedImageRectHeight;
	}
	g_views[0].width = aw;
	g_views[0].height = ah;

	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = g_swapchain_format;
	ci.sampleCount = view_configs[0].recommendedSwapchainSampleCount;
	ci.width = aw;
	ci.height = ah;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	res = xrCreateSwapchain(g_session, &ci, &g_views[0].swapchain);
	if (res != XR_SUCCESS) {
		log_xr_result("xrCreateSwapchain", res);
		return false;
	}
	uint32_t img_count = 0;
	res = xrEnumerateSwapchainImages(g_views[0].swapchain, 0, &img_count, nullptr);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainImages(count)", res);
		return false;
	}
	if (img_count > 8) {
		img_count = 8;
	}
	for (uint32_t j = 0; j < img_count; ++j) {
		g_views[0].images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	res = xrEnumerateSwapchainImages(
	    g_views[0].swapchain, img_count, &img_count,
	    reinterpret_cast<XrSwapchainImageBaseHeader *>(g_views[0].images));
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainImages(fill)", res);
		return false;
	}
	g_views[0].image_count = img_count;
	g_views[1] = g_views[0]; // alias: all projection views share the one atlas swapchain
	LOGI("Atlas swapchain: %ux%u, %u images", aw, ah, img_count);
	return true;
}

bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {};
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	ci.poseInReferenceSpace.position = {0, 0, 0};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

// Initialize the 3DGS renderer against the runtime's Vulkan resources.
bool
gs_init()
{
	if (!g_model.init(g_vk_instance, g_vk_phys_device, g_vk_device, g_vk_queue,
	               g_vk_queue_family, g_views[0].width, g_views[0].height)) {
		LOGE("ModelRenderer::init failed");
		return false;
	}
	// Full quality: all 177k gaussians at full resolution. The fps is capped
	// by the runtime weave/present (~14 fps), not app compute — render-scale,
	// fewer gaussians, and removing the CPU-readback round-trips all left fps
	// unchanged — so there's no perf reason to subsample. (setGaussianStride /
	// setRenderScale knobs remain for future tuning.)
	g_model_ready = true;
	LOGI("ModelRenderer initialized (%ux%u/eye, full quality 177k)", g_views[0].width, g_views[0].height);
	return true;
}

// Load a glTF from a filesystem path into the renderer + auto-frame. Safe to
// call between frames on the android_main thread (same thread that renders) —
// it waits for device idle before rebuilding the renderer's buffers.
bool
load_model_path(const char *path)
{
	if (!g_model_ready) {
		return false;
	}
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);  // no GPU work touching the old model
	}
	if (!g_model.loadModel(path)) {
		LOGE("ModelRenderer::loadModel failed for %s", path);
		return false;
	}
	LOGI("Loaded model: %s", path);

	// Auto-frame: recenter on the robust scene centroid and pick a push-back
	// distance from the scene extent so it starts at a comfortable size.
	float ext[3] = {1.0f, 1.0f, 1.0f};
	if (g_model.getRobustSceneBounds(0.05f, 0.95f, g_scene_center, ext)) {
		float maxe = ext[0] > ext[1] ? ext[0] : ext[1];
		if (ext[2] > maxe) {
			maxe = ext[2];
		}
		// #568: recenter on the ANIMATED visual centre, not the static AABB
		// centre. The tiger plays a clip; its animated mass centre differs from
		// the bind-pose AABB centre, so recentering by the static centre rides
		// the animated tiger high (top truncated). getAnimatedAnchor is the
		// clip-wide animated centre (visual-centre corrected) — the Android
		// analog of Windows positioning the rig at the box centre after the
		// animation starts. Falls back to the static centre if unavailable.
		float anch[3];
		if (g_model.getAnimatedAnchor(anch)) {
			g_scene_center[0] = anch[0];
			g_scene_center[1] = anch[1];
			g_scene_center[2] = anch[2];
		}
		g_fit_scale = (maxe > 1e-4f) ? kTargetSize / maxe : 1.0f;
		// #570: match macOS/Windows — render the model at WORLD scale (1.0) and
		// let virtual_display_height (= modelHeight/0.9, set below) drive the 90%
		// fill, instead of pre-scaling the model. Pre-scaling decoupled the model
		// size from vHeight and mis-framed the tiger once the runtime off-axis-
		// frames the zone. Pinch zoom still rides g_scene_scale.
		g_scene_scale.store(1.0f, std::memory_order_relaxed);
		g_scene_push.store(g_has_view_rig ? 0.0f : 0.45f, std::memory_order_relaxed);
		// 90% fill: the avatar occupies 90% of the virtual-display (zone) height
		// at start — vHeight = modelHeight / 0.90 = modelHeight × 1.111 (Windows
		// kAutoFitVerticalComfort). The model is at WORLD scale (ext[1] m tall),
		// NOT scaled by g_fit_scale, so vh = ext[1] × 1.111 — the old
		// ext[1]×g_fit_scale×1.4 conflated an unapplied scale → ~76% + off.
		const float vh = ext[1] * 1.111f;
		g_rig_vh = (vh > 1e-3f) ? vh : kTargetSize * 1.111f;
		LOGI("scene center=(%.2f,%.2f,%.2f) extent=(%.2f,%.2f,%.2f) push=%.2f rig_vh=%.2f",
		     g_scene_center[0], g_scene_center[1], g_scene_center[2],
		     ext[0], ext[1], ext[2], g_scene_push.load(std::memory_order_relaxed), g_rig_vh);
	}
	g_scene_loaded.store(true, std::memory_order_relaxed);
	return true;
}

// Copy kModels[idx] out of the APK assets into app-private storage (the glTF
// loader takes a filesystem path), then load it via load_model_path.
bool
load_model_index(struct android_app *app, int idx)
{
	if (idx < 0 || idx >= kModelCount) {
		idx = 0;
	}
	const char *name = kModels[idx];
	AAssetManager *mgr = app->activity->assetManager;

	// Stage sidecar files (e.g. the FBX's rgb.jpg texture) next to the model so
	// the loader resolves their relative paths from app-private storage.
	for (int s = 0; s < kSidecarCount; ++s) {
		AAsset *sc = AAssetManager_open(mgr, kModelSidecars[s], AASSET_MODE_BUFFER);
		if (sc == nullptr) {
			continue;
		}
		const void *sbuf = AAsset_getBuffer(sc);
		const off_t slen = AAsset_getLength(sc);
		std::string spath = std::string(app->activity->internalDataPath) + "/" + kModelSidecars[s];
		if (sbuf != nullptr && slen > 0) {
			FILE *sf = std::fopen(spath.c_str(), "wb");
			if (sf != nullptr) {
				std::fwrite(sbuf, 1, (size_t)slen, sf);
				std::fclose(sf);
			}
		}
		AAsset_close(sc);
	}

	AAsset *asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
	if (asset == nullptr) {
		LOGE("%s not found in assets", name);
		return false;
	}
	const void *buf = AAsset_getBuffer(asset);
	const off_t len = AAsset_getLength(asset);
	std::string path = std::string(app->activity->internalDataPath) + "/" + name;
	bool ok = false;
	if (buf != nullptr && len > 0) {
		FILE *f = std::fopen(path.c_str(), "wb");
		if (f != nullptr) {
			ok = std::fwrite(buf, 1, (size_t)len, f) == (size_t)len;
			std::fclose(f);
		}
	}
	AAsset_close(asset);
	if (!ok) {
		LOGE("failed to stage %s to %s", name, path.c_str());
		return false;
	}
	if (!load_model_path(path.c_str())) {
		return false;
	}
	g_model_index.store(idx, std::memory_order_relaxed);
	return true;
}

void
handle_session_state(XrSessionState new_state)
{
	g_session_state = new_state;
	switch (new_state) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo begin = {};
		begin.type = XR_TYPE_SESSION_BEGIN_INFO;
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		XrResult res = xrBeginSession(g_session, &begin);
		log_xr_result("xrBeginSession", res);
		if (res == XR_SUCCESS) {
			g_session_running = true;
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING: {
		XrResult res = xrEndSession(g_session);
		log_xr_result("xrEndSession", res);
		g_session_running = false;
		break;
	}
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		g_exit_requested = true;
		break;
	default:
		break;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}
		if (res != XR_SUCCESS) {
			log_xr_result("xrPollEvent", res);
			break;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
			if (e->session == g_session) {
				LOGI("session state -> %d", (int)e->state);
				handle_session_state(e->state);
			}
		} else if (ev.type == (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT) {
			const auto *e = reinterpret_cast<const XrEventDataRenderingModeChangedEXT *>(&ev);
			LOGI("rendering mode changed: %u -> %u", e->previousModeIndex, e->currentModeIndex);
			g_rmode_current.store(e->currentModeIndex, std::memory_order_relaxed);
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_exit_requested = true;
		}
	}
}

bool
render_frame()
{
	XrFrameWaitInfo wait_info = {};
	wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
	XrFrameState frame_state = {};
	frame_state.type = XR_TYPE_FRAME_STATE;
	XrResult res = xrWaitFrame(g_session, &wait_info, &frame_state);
	if (res != XR_SUCCESS) {
		log_xr_result("xrWaitFrame", res);
		return false;
	}
	XrFrameBeginInfo begin_info = {};
	begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
	res = xrBeginFrame(g_session, &begin_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrBeginFrame", res);
		return false;
	}

	XrCompositionLayerProjectionView projection_views[kViewCount] = {};
	bool rendered = false;
	uint32_t submitted_view_count = kViewCount;
	// Display-zones framing (#568): the SAME zone chains on the zone-scoped
	// locate AND the submitted projection layer. Declared at frame scope so the
	// submit can reference it; populated in the locate block when zones are live.
	XrDisplayZoneEXT tiger_zone = {XR_TYPE_DISPLAY_ZONE_EXT};
	bool zones_frame = false;
	if (frame_state.shouldRender && g_scene_loaded.load(std::memory_order_relaxed)) {
		XrViewState view_state = {};
		view_state.type = XR_TYPE_VIEW_STATE;
		XrViewLocateInfo locate_info = {};
		locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = g_app_space;

		// W7 (#396): chain the display rig so the runtime returns render-ready
		// off-axis views (server-side Kooima over IPC on Android, #510/#513).
		// Identity rig pose — this app orbits the model, not the camera. The
		// raw channel reports the DP's display-space eyes + tracking state.
		XrDisplayRigEXT display_rig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrViewDisplayRawEXT view_raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		// #570: match macOS/Windows — the virtual display is centred ON the tiger
		// (the model's animated scene centre), not at the world origin. The model
		// is rendered in place at the same centre (build_splat_model), so the
		// tiger sits at the centre of its virtual display at 90% of vHeight.
		//
		// Touch-drag camera controls (Windows parity, windows/main.cpp:1272-1290):
		// the rig (= virtual display plane the server Kooima frames around) is offset
		// from the animated anchor by the accumulated drag — horizontal = lateral
		// strafe (kPanSign matches Windows' D-is-right), vertical = depth dolly. The
		// model render stays at g_scene_center (build_splat_model `place`), so the
		// rig→model offset is what slides/pops the avatar. At zero drag the rig
		// coincides with the model — identical to the pre-control framing.
		constexpr float kPanSign = -1.0f;  // D = avatar right; flip on device if reversed
		const XrPosef rig_pose = {
		    {0.0f, 0.0f, 0.0f, 1.0f},
		    {g_scene_center[0] + kPanSign * g_cam_pan_x.load(std::memory_order_relaxed),
		     g_scene_center[1],
		     g_scene_center[2] + g_cam_dolly_z.load(std::memory_order_relaxed)}};
		const float rig_vh = g_rig_vh;
		if (g_has_view_rig) {
			display_rig.pose = rig_pose;
			display_rig.virtualDisplayHeight = rig_vh;  // pinch zoom scales the model, not vH
			display_rig.ipdFactor = 1.0f;
			display_rig.parallaxFactor = 1.0f;
			display_rig.perspectiveFactor = 1.0f;
			locate_info.next = &display_rig;
			view_state.next = &view_raw;

			// Chain the bottom-75% tiger zone in FRONT of the rig (#568): the
			// runtime frames the off-axis views to the zone rect and composites
			// the rendered tile into that sub-region of the canvas. Rect is in
			// canvas px (y-down) from last frame's raw channel; one-frame-stale
			// is invisible. Frame 0 (no canvas yet) stays rig-only full-canvas.
			if (g_zones_active) {
				const uint32_t cw = g_win_px_w.load(std::memory_order_relaxed);
				const uint32_t ch = g_win_px_h.load(std::memory_order_relaxed);
				if (cw > 0 && ch > 0) {
					tiger_zone.zoneId = 1;
					tiger_zone.rect.offset = {0, (int32_t)((float)ch * kBubbleBandFrac)};
					tiger_zone.rect.extent = {(int32_t)cw,
					                          (int32_t)((float)ch * (1.0f - kBubbleBandFrac))};
					tiger_zone.next = &display_rig;
					locate_info.next = &tiger_zone;
					zones_frame = true;
				}
			}
		}

		XrView views[kViewCount] = {};
		for (uint32_t i = 0; i < kViewCount; ++i) {
			views[i].type = XR_TYPE_VIEW;
		}
		uint32_t located = 0;
		res = xrLocateViews(g_session, &locate_info, &view_state, kViewCount, &located, views);
		// Multiview-tiling invariant: submit the ACTIVE mode's view count — 2D = 1
		// tile (1x1), 3D = 2 tiles (2x1). Each tile is rendered at the active tile
		// size (display × view_scale; computed below once g_win_px is fresh) into a
		// sub-rect of the worst-case swapchain, and submitted as subImage.imageRect.
		// The runtime z-fix (displayxr-runtime#538) keeps the 2D eye distance right.
		uint32_t view_count = located < kViewCount ? located : kViewCount;
		{
			const uint32_t mode = g_rmode_current.load(std::memory_order_relaxed);
			const uint32_t mvc = (mode < 8 && g_rmode_vc[mode] > 0) ? g_rmode_vc[mode] : view_count;
			if (mvc < view_count)
				view_count = mvc;
		}
		if (res == XR_SUCCESS && view_count >= 1) {
			DXR_HW_DBG_ONCE("first xrLocateViews success");
			submitted_view_count = view_count;
			if (g_has_view_rig) {
				// Live window px for the button bar (tap → fraction mapping
				// and bar aspect) — tracks rotation via the raw channel.
				if (view_raw.canvasRectPx.extent.width > 0 && view_raw.canvasRectPx.extent.height > 0) {
					g_win_px_w.store((uint32_t)view_raw.canvasRectPx.extent.width,
					                 std::memory_order_relaxed);
					g_win_px_h.store((uint32_t)view_raw.canvasRectPx.extent.height,
					                 std::memory_order_relaxed);
				}
			}
			// Active-mode tile size = display (held orientation, via the live canvas
			// px) × the mode's view_scale; tiles are laid out per the mode's grid
			// (cols×rows) inside the one worst-case atlas. tile_w/h drive both the
			// per-tile render viewport and each view's submitted subImage.imageRect.
			uint32_t tile_w = 0, tile_h = 0, cols = 1, rows = 1;
			{
				const uint32_t mode = g_rmode_current.load(std::memory_order_relaxed);
				cols = (mode < 8 && g_rmode_cols[mode] > 0) ? g_rmode_cols[mode] : 1;
				rows = (mode < 8 && g_rmode_rows[mode] > 0) ? g_rmode_rows[mode] : 1;
				uint32_t dw = g_win_px_w.load(std::memory_order_relaxed);
				uint32_t dh = g_win_px_h.load(std::memory_order_relaxed);
				if (dw == 0 || dh == 0) {
					dw = g_views[0].width / cols;
					dh = g_views[0].height / rows;
				}
				// #568: under display-zones the runtime frames the Kooima FOV to the
				// ZONE rect (bottom-75%), so the render tile must match the ZONE
				// aspect, not the full canvas — otherwise a full-window-aspect tile
				// is scaled into the shorter zone rect → vertical squish. Size from
				// the zone extent (== xrGetDisplayZoneRecommendedViewSizeEXT, which
				// returns the rect extent 1:1).
				if (zones_frame) {
					dw = (uint32_t)tiger_zone.rect.extent.width;
					dh = (uint32_t)tiger_zone.rect.extent.height;
				}
				const float sx = (mode < 8 && g_rmode_scale_x[mode] > 0.0f) ? g_rmode_scale_x[mode] : 1.0f;
				const float sy = (mode < 8 && g_rmode_scale_y[mode] > 0.0f) ? g_rmode_scale_y[mode] : 1.0f;
				tile_w = (uint32_t)((float)dw * sx);
				tile_h = (uint32_t)((float)dh * sy);
				if (tile_w == 0) tile_w = dw;
				if (tile_h == 0) tile_h = dh;
				// Tiles must not overflow the atlas (cols×tile_w ≤ atlas_w, etc).
				const uint32_t max_tw = g_views[0].width / cols;
				const uint32_t max_th = g_views[0].height / rows;
				if (tile_w > max_tw) tile_w = max_tw;
				if (tile_h > max_th) tile_h = max_th;
			}
			// Face-the-viewer billboard yaw (windows/main.cpp:1386-1436). Head
			// centroid from the raw display-space eyes (rawEyes are display-CENTRE
			// origin, +X right +Z toward viewer — and the tiger zone is full-width
			// bottom-75%, so the zone centre == display centre and NO canvas rebase
			// is needed, unlike the panel-relative Windows path). Only chase while
			// eye tracking is LOCKED; hold facing-forward (yaw 0) during warmup.
			// Time-based exponential smoothing → frame-rate independent.
			static constexpr float FACE_YAW_SIGN = 1.0f;  // NP02J-confirmed (David, 2026-06-14)
			static float s_face_yaw = 0.0f;
			{
				float cx = 0.0f, cz = 0.0f;
				const uint32_t ne = view_raw.eyeCountOutput;
				if (ne >= 1) {
					for (uint32_t e = 0; e < ne; ++e) {
						cx += view_raw.rawEyes[e].x;
						cz += view_raw.rawEyes[e].z;
					}
					cx /= (float)ne;
					cz /= (float)ne;
				}
				const float hz_abs = fabsf(cz) > 1.0e-3f ? fabsf(cz) : 1.0e-3f;
				const float target_yaw = FACE_YAW_SIGN * atan2f(cx, hz_abs);
				const float tau = 0.04f, dt = 1.0f / 60.0f;  // settle ≈ 3·tau
				float a = 1.0f - expf(-dt / tau);
				a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
				if (view_raw.isTracking) {
					float dy = target_yaw - s_face_yaw;
					while (dy > 3.14159265f) dy -= 6.28318531f;
					while (dy < -3.14159265f) dy += 6.28318531f;
					s_face_yaw += dy * a;
				}
			}
			const float yaw = s_face_yaw;
			// #568: track the ANIMATED visual anchor each frame so the avatar
			// stays centred in the zone as the clip plays — the load-time static
			// AABB centre mis-centres the animated tiger (rides high / top-
			// truncated). getAnimatedAnchor is a smoothed live animated-centroid
			// (valid only after the first animated frame, hence per-frame here, not
			// at load). Matches Windows positioning the rig at the animated box
			// centre. build_splat_model below recenters by g_scene_center.
			{
				float anch[3];
				if (g_model.getAnimatedAnchor(anch)) {
					g_scene_center[0] = anch[0];
					g_scene_center[1] = anch[1];
					g_scene_center[2] = anch[2];
				}
			}
			const Mat4 splat_model = build_splat_model(yaw);
			// Advance glTF animation (bind pose if the model has none). ~60 fps dt.
			g_model.updateAnimation(1.0f / 60.0f);

			// #568 rig framing: under XR_EXT_view_rig the XrView pose is already
			// render-ready (off-axis Kooima), so the renderer must consume it in
			// the PLAIN convention — no view-stage Y-flip; the Vulkan Y-flip is the
			// negative-height viewport in renderEye. Without this the legacy
			// convention (which expects a Y-mirrored display pose) mis-applies the
			// off-axis vertical framing → the avatar rides high ("only legs").
			// Mirrors the Windows zones path: setPlainViewConvention(zonesFrame).
			g_model.setPlainViewConvention(g_has_view_rig);

			// Per-view matrices captured for the silhouette pass (gesture gate /
			// click-through) after the atlas render — same evM/projM/clip the tiger
			// is drawn with, so the coverage matches the on-screen tiger exactly.
			Mat4 sil_evM[kViewCount];
			Mat4 sil_projM[kViewCount];
			float sil_clip[kViewCount] = {0};

			// ONE atlas swapchain (multiview-tiling invariant): acquire once, render
			// each tile into its (cols×rows) layout offset, release once. All views
			// reference g_views[0]; each signals its tile via subImage.imageRect and
			// the compositor maps eye→destination tile by index. renderEye preserves
			// earlier tiles when viewportX!=0 (first tile clears with UNDEFINED).
			XrSwapchainImageAcquireInfo acq = {};
			acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
			uint32_t img_idx = 0;
			res = xrAcquireSwapchainImage(g_views[0].swapchain, &acq, &img_idx);
			if (res == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wait_img = {};
				wait_img.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				wait_img.timeout = XR_INFINITE_DURATION;
				res = xrWaitSwapchainImage(g_views[0].swapchain, &wait_img);
			}
			if (res == XR_SUCCESS) {
				for (uint32_t i = 0; i < view_count; ++i) {
					const uint32_t tile_x = (i % cols) * tile_w;
					const uint32_t tile_y = (i / cols) * tile_h;

					Mat4 viewM = view_matrix_from_pose(views[i].pose);
					Mat4 evM = mat4_mul(viewM, splat_model);  // apply splat model
					Mat4 projM;
					// Foreground-only clip (windows/main.cpp:1640-1668): in transparent
					// mode cull geometry behind the virtual display plane (ZDP) so only
					// popping-out content shows. clipFar rides in the renderer UBO
					// (lightDir.w, view-space); 0 = off. The avatar always renders
					// transparent here, so it collapses to the ez>0.2m guard (never cull
					// at/behind near). Threaded into renderEye below.
					float clip_far = 0.0f;
					if (g_has_view_rig) {
						// Render-ready off-axis FOV from the rig — pass it through
						// (aspect -1: the symmetric override would destroy the
						// off-axis skew). ZDP-anchored clip about the virtual display
						// plane: near = ez - vH; transparent far = ez (clip at the ZDP,
						// matching Windows — opaque would use ez + 1000·vH).
						const float ez = rig_local_eye_z(rig_pose, views[i].pose.position);
						const float near_z = (ez - rig_vh > 1.0e-4f) ? (ez - rig_vh) : 1.0e-4f;
						const float far_z = ez;
						clip_far = (ez > 0.2f) ? ez : 0.0f;
						XrFovf eff_fov = views[i].fov;
						// #568: the Android OOP runtime returns a FULL-CANVAS rig FOV
						// (its zone canvas rebase is D3D11-gated → no-op on Android),
						// so reframe the Kooima to the zone rect here; otherwise the
						// full-window-framed content squishes into the shorter zone.
						// Guard: only when the returned FOV aspect is closer to the
						// full canvas than the zone — so this is a no-op (no double-
						// apply) if the runtime later zone-frames the FOV itself.
						if (zones_frame) {
							const uint32_t cw = g_win_px_w.load(std::memory_order_relaxed);
							const uint32_t ch = g_win_px_h.load(std::memory_order_relaxed);
							if (cw > 0 && ch > 0 && tiger_zone.rect.extent.height > 0) {
								const float tw = std::tan(eff_fov.angleRight) - std::tan(eff_fov.angleLeft);
								const float th = std::tan(eff_fov.angleUp) - std::tan(eff_fov.angleDown);
								const float fov_aspect = (th != 0.0f) ? tw / th : 0.0f;
								const float canvas_aspect = (float)cw / (float)ch;
								const float zone_aspect = (float)tiger_zone.rect.extent.width /
								                          (float)tiger_zone.rect.extent.height;
								if (fabsf(fov_aspect - canvas_aspect) < fabsf(fov_aspect - zone_aspect)) {
									eff_fov = zone_subfrustum_fov(
									    views[i].fov, cw, ch,
									    tiger_zone.rect.offset.x, tiger_zone.rect.offset.y,
									    tiger_zone.rect.extent.width, tiger_zone.rect.extent.height);
								}
							}
						}
						projM = projection_matrix_from_fov(eff_fov, -1.0f, near_z, far_z);
					} else {
						const float aspect = (float)tile_w / (float)tile_h;
						projM = projection_matrix_from_fov(views[i].fov, aspect, 0.01f, 100.0f);
					}
					sil_evM[i] = evM;
					sil_projM[i] = projM;
					sil_clip[i] = clip_far;

					// Render tile i into its (tile_x,tile_y) sub-rect of the atlas.
					// #568: transparentBg=true → the renderer clears the tile to
					// premultiplied (0,0,0,0) and skips the skybox, so everything
					// around the avatar is transparent. The runtime's alpha-gate
					// (Leia DP) / sim passthrough carries that alpha to the
					// translucent SurfaceView → the live screen shows through.
					g_model.renderEye(
					    g_views[0].images[img_idx].image, g_swapchain_format,
					    g_views[0].width, g_views[0].height,
					    tile_x, tile_y, tile_w, tile_h,
					    evM.m, projM.m,
					    /*transparentBg*/ true, /*clipFarViewSpace*/ clip_far);

					projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
					projection_views[i].pose = views[i].pose;
					projection_views[i].fov = views[i].fov;
					projection_views[i].subImage.swapchain = g_views[0].swapchain;
					projection_views[i].subImage.imageRect.offset = {(int32_t)tile_x, (int32_t)tile_y};
					projection_views[i].subImage.imageRect.extent = {(int32_t)tile_w, (int32_t)tile_h};
					projection_views[i].subImage.imageArrayIndex = 0;
				}
				XrSwapchainImageReleaseInfo rel = {};
				rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				res = xrReleaseSwapchainImage(g_views[0].swapchain, &rel);
			} else {
				log_xr_result("xrAcquire/WaitSwapchainImage", res);
			}
			rendered = (res == XR_SUCCESS);
			// Refresh the gesture/click-through silhouette (throttled ~10 Hz —
			// the tiger moves slowly and only touch-DOWN samples it; each tick is
			// 1-2 extra downscaled model draws + a readback). Zone frames only.
			if (rendered && zones_frame && (g_frame_count % 6) == 0) {
				update_silhouette(g_views[0].width, g_views[0].height,
				    tiger_zone.rect.offset.x, tiger_zone.rect.offset.y,
				    tiger_zone.rect.extent.width, tiger_zone.rect.extent.height,
				    sil_evM, sil_projM, sil_clip, view_count);
			}
		} else {
			log_xr_result("xrLocateViews", res);
		}
	}

	XrCompositionLayerProjection projection_layer = {};
	projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection_layer.space = g_app_space;
	projection_layer.viewCount = submitted_view_count;
	projection_layer.views = projection_views;
	// Chain the SAME tiger zone on the submitted layer (#568): tells the runtime
	// to composite the rendered view tile into the bottom-75% zone rect of the
	// output canvas. next must be NULL on the submit chain (the rig was only for
	// the locate). Skipped on a non-zones frame (frame 0 / zones unsupported).
	if (zones_frame) {
		tiger_zone.next = nullptr;
		projection_layer.next = &tiger_zone;
	}
	// #568 speech bubble — an XR_EXT_local_3d_zone Local2D layer placed in the
	// top-25% 2D band (above the tiger zone). Static greeting uploaded once at
	// init; the runtime blits the released image into `rect` each frame. Only in
	// a zones frame (the band only exists when the tiger is zone-confined).
	XrCompositionLayerLocal2DEXT bubble_layer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
	bool bubble_active = false;
	if (zones_frame && g_bubble.ready) {
		const uint32_t cw = g_win_px_w.load(std::memory_order_relaxed);
		const uint32_t ch = g_win_px_h.load(std::memory_order_relaxed);
		if (cw > 0 && ch > 0) {
			const float band_h = (float)ch * kBubbleBandFrac;  // top 25%
			const float bubble_aspect = (float)kBubbleTexH / (float)kBubbleTexW;
			// Size the bubble to 70% width, but CLAMP so its aspect-locked height
			// fits the band. In landscape the band is short (25% of the small
			// dimension) while cw is large, so a pure width-relative size made the
			// bubble taller than the band → it spilled into the tiger zone and the
			// 2D region read as ">25%" (#570). Constrain by height first, then
			// width, whichever binds.
			float bwf = (float)cw * 0.70f;
			float bhf = bwf * bubble_aspect;
			const float max_bh = band_h * 0.90f;  // leave a 10% margin in the band
			if (bhf > max_bh) {
				bhf = max_bh;
				bwf = bhf / bubble_aspect;
			}
			const int bw = (int)bwf;
			const int bh = (int)bhf;
			int bx0 = (int)(((float)cw - (float)bw) * 0.5f);
			int by0 = (int)((band_h - (float)bh) * 0.5f);       // centred in the band
			const int top_margin = (int)(band_h * 0.05f);
			if (by0 < top_margin) {
				by0 = top_margin;
			}
			bubble_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			bubble_layer.subImage.swapchain = g_bubble.swapchain;
			bubble_layer.subImage.imageRect.offset = {0, 0};
			bubble_layer.subImage.imageRect.extent = {(int32_t)kBubbleTexW, (int32_t)kBubbleTexH};
			bubble_layer.subImage.imageArrayIndex = 0;
			bubble_layer.rect.offset = {bx0, by0};
			bubble_layer.rect.extent = {bw, bh};
			bubble_active = true;
		}
	}

	// Layer order: projection/zone first, then the #568 speech bubble on top.
	const XrCompositionLayerBaseHeader *layers[2];
	uint32_t base_count = 0;
	layers[base_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer);
	if (bubble_active) {
		layers[base_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&bubble_layer);
	}

	XrFrameEndInfo end_info = {};
	end_info.type = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = rendered ? base_count : 0;
	end_info.layers = rendered ? layers : nullptr;
	res = xrEndFrame(g_session, &end_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEndFrame", res);
		return false;
	}

	g_frame_count++;
	if ((g_frame_count % 120) == 0) {
		static auto last = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(now - last).count() / 120.0;
		last = now;
		LOGI("frame %llu  ~%.1f ms/frame (%.1f fps)", (unsigned long long)g_frame_count,
		     ms, ms > 0.0 ? 1000.0 / ms : 0.0);
	}
	return true;
}

void
destroy_all()
{
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);
	}
	if (g_model_ready) {
		g_model.cleanup();
		g_model_ready = false;
	}
	hud_bar_destroy(g_bubble);
	if (g_sil_mapped) {
		vkUnmapMemory(g_vk_device, g_sil_readback.memory);
		g_sil_mapped = nullptr;
	}
	if (g_sil_readback.buffer != VK_NULL_HANDLE) modelDestroyBuffer(g_vk_device, g_sil_readback);
	if (g_sil_image.image != VK_NULL_HANDLE) modelDestroyImage(g_vk_device, g_sil_image);
	if (g_sil_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(g_vk_device, g_sil_pool, nullptr);
		g_sil_pool = VK_NULL_HANDLE;
	}
	if (g_session != XR_NULL_HANDLE) {
		xrDestroySession(g_session);
		g_session = XR_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < kViewCount; ++i) {
		if (g_views[i].swapchain != XR_NULL_HANDLE) {
			xrDestroySwapchain(g_views[i].swapchain);
			g_views[i].swapchain = XR_NULL_HANDLE;
		}
	}
	if (g_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(g_app_space);
		g_app_space = XR_NULL_HANDLE;
	}
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDestroyDevice(g_vk_device, nullptr);
		g_vk_device = VK_NULL_HANDLE;
	}
	if (g_vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g_vk_instance, nullptr);
		g_vk_instance = VK_NULL_HANDLE;
	}
	if (g_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g_instance);
		g_instance = XR_NULL_HANDLE;
	}
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
		if (g_instance == XR_NULL_HANDLE) {
			bool ok =
			    create_instance(app) &&
			    query_system_and_graphics_reqs() &&
			    create_vulkan_instance() &&
			    pick_physical_device() &&
			    create_vulkan_device() &&
			    create_session() &&
			    // Enumerate modes BEFORE create_swapchains so the worst-case tile
			    // (g_worst_tile_edge) + per-mode scales are known when the (never-
			    // reallocated) swapchain is sized. (multiview-tiling invariant.)
			    enumerate_rendering_modes() &&
			    create_swapchains() &&
			    create_reference_space() &&
			    gs_init() &&
			    load_model_index(app, 0);
			if (ok && g_has_local_3d_zone) {
				// #568 speech bubble (Local2D layer in the top-25% band). Non-fatal:
				// on failure the bubble simply doesn't draw. Rasterized once here.
				if (hud_bar_init(g_bubble, g_session, g_vk_phys_device, g_vk_device,
				                 g_vk_queue, g_vk_queue_family, g_swapchain_format,
				                 kBubbleTexW, kBubbleTexH, 56.0f)) {
					hud_bar_render_bubble(g_bubble, kBubbleText);
					hud_bar_upload(g_bubble, 1.0f);
					LOGI("#568 speech bubble ready (%ux%u)", kBubbleTexW, kBubbleTexH);
				}
			}
			LOGI(ok ? "Bring-up complete." : "Bring-up failed; see logs.");
		}
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_all();
		break;
	default:
		break;
	}
}

} // namespace

// ─── JNI bridge to MainActivity ──────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_avatar_1vk_1android_MainActivity_nativeSetRotation(
    JNIEnv * /*env*/, jobject /*thiz*/, jint rotation)
{
	g_display_rotation.store(rotation & 3, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_avatar_1vk_1android_MainActivity_nativeRuntimeUnavailable(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return g_runtime_unavailable.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_avatar_1vk_1android_MainActivity_nativeXrReady(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return (g_instance != XR_NULL_HANDLE) ? JNI_TRUE : JNI_FALSE;
}

// #558 overlay mode (debug.dxr.overlay): when set, MainActivity starts a
// keep-alive foreground service + moveTaskToBack so the launcher resumes and the
// tiger floats on the runtime's service overlay. Reads the prop directly so it
// doesn't race android_main's read on the render thread.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_avatar_1vk_1android_MainActivity_nativeOverlayMode(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	char prop[PROP_VALUE_MAX] = {};
	return (__system_property_get("debug.dxr.overlay", prop) > 0 && prop[0] == '1') ? JNI_TRUE
	                                                                                 : JNI_FALSE;
}

// Touch bridge, fed from Java. The runtime's MonadoView overlay is the only
// window that receives touch (it covers our NativeActivity); David's #499 design
// forwards each event to the host Activity via dispatchTouchEvent. But a
// NativeActivity's native InputQueue (→ app->onInputEvent) is NOT driven by
// Activity.dispatchTouchEvent — so we capture the forwarded MotionEvent in
// MainActivity.dispatchTouchEvent and bridge the coordinates down here instead.
// One finger = camera controls (depth dolly + lateral strafe, Windows parity);
// two fingers = pinch-to-zoom. action = MotionEvent.getActionMasked(); count =
// pointerCount.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_avatar_1vk_1android_MainActivity_nativeOnTouch(
    JNIEnv * /*env*/, jobject /*thiz*/, jint action, jint count,
    jfloat x0, jfloat y0, jfloat x1, jfloat y1)
{
	static float pinch_last = 0.0f;
	static float drag_x = 0.0f, drag_y = 0.0f;
	static bool drag_valid = false;       // a clean single-finger gesture is in progress
	static bool gesture_on_tiger = false; // did this gesture start on the tiger silhouette?
	// AMOTION_EVENT_ACTION_* values match MotionEvent.ACTION_* (0=DOWN,1=UP,2=MOVE,…).
	constexpr int kDown = 0, kUp = 1, kMove = 2;

	// Gate to the tiger: decide on the FIRST finger down whether this gesture
	// controls the avatar (initiated over the silhouette). A gesture that began
	// on the tiger keeps control even if the finger later slides off; one that
	// began off it is ignored entirely (next step: pass it through to whatever
	// is behind). Fail-open until the first coverage is published.
	if (action == kDown) gesture_on_tiger = sil_hit(x0, y0);
	if (!gesture_on_tiger) {
		if (action == kUp) {
			drag_valid = false;
			pinch_last = 0.0f;
		}
		return;
	}

	if (count >= 2) {
		// ── two fingers: pinch-to-zoom (scale around the auto-fit size) ──
		drag_valid = false;  // suppress rotation while pinching
		const float dx = x0 - x1, dy = y0 - y1;
		const float dist = std::sqrt(dx * dx + dy * dy);
		if (action == kMove && pinch_last > 1.0f) {
			float s = g_scene_scale.load(std::memory_order_relaxed) * (dist / pinch_last);
			const float lo = g_fit_scale * 0.15f, hi = g_fit_scale * 8.0f;
			if (s < lo) s = lo;
			if (s > hi) s = hi;
			g_scene_scale.store(s, std::memory_order_relaxed);
		}
		pinch_last = dist;
		return;
	}
	pinch_last = 0.0f;  // <2 fingers

	// ── one finger: camera controls (windows/main.cpp A/D + W/S) ──
	// Horizontal drag → lateral strafe (cameraPosX); vertical drag → depth dolly
	// (cameraPosZ). The avatar never rotates. Per-pixel gains are the on-device
	// tuning knobs (screencap is blind — David eyeballs); flip a sign here or
	// kPanSign / the dolly sign in the render loop if a direction feels reversed.
	constexpr float kPanPerPx = 0.002f;    // lateral units per px (≈500px → 1 unit)
	constexpr float kDollyPerPx = 0.004f;  // depth   units per px (≈250px → 1 unit)
	if (action == kDown) {
		drag_x = x0;
		drag_y = y0;
		drag_valid = true;
	} else if (action == kMove && drag_valid) {
		const float dx = x0 - drag_x, dy = y0 - drag_y;
		drag_x = x0;
		drag_y = y0;
		g_cam_pan_x.store(g_cam_pan_x.load(std::memory_order_relaxed) + dx * kPanPerPx,
		                  std::memory_order_relaxed);
		g_cam_dolly_z.store(g_cam_dolly_z.load(std::memory_order_relaxed) + dy * kDollyPerPx,
		                    std::memory_order_relaxed);
	} else if (action == kUp) {
		drag_valid = false;
	}
}

// Double-tap (from Java's GestureDetector) recenters the camera (pan/dolly/zoom
// back to the framed default). Just records the request; the reset runs on the
// android_main thread (no Vulkan work, no model reload).
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_avatar_1vk_1android_MainActivity_nativeResetView(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	g_reset_view_request.store(true, std::memory_order_relaxed);
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("avatar_vk_android: android_main entered");
	{
		// #558 overlay mode: render while backgrounded so the tiger stays on the
		// service overlay over the live launcher. `setprop debug.dxr.overlay 1`.
		char prop[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.dxr.overlay", prop) > 0 && prop[0] == '1') {
			g_overlay_mode = true;
			LOGI("overlay mode ON: render while backgrounded (foreground-service model)");
		}
	}
	app->onAppCmd = handle_cmd;
	// Touch is NOT consumed via app->onInputEvent: the runtime's MonadoView
	// overlay covers our window, so a NativeActivity never sees native input.
	// Gestures arrive via MainActivity.dispatchTouchEvent → nativeOnTouch JNI.

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed");
	}

	while (true) {
		const int poll_timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(poll_timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				destroy_all();
				return;
			}
		}
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) {
				destroy_all();
				return;
			}
			// #533 test hook: absolute mode select via adb. `setprop
			// debug.dxr.mode N` → request mode N (0=2D, 1=3D). Re-request only on
			// change. (The only mode control now that the button bar is gone.)
			{
				static int last_prop_mode = -1;
				char prop[PROP_VALUE_MAX] = {0};
				if (__system_property_get("debug.dxr.mode", prop) > 0) {
					int want = atoi(prop);
					if (want >= 0 && want != last_prop_mode && g_pfnReqMode != nullptr &&
					    (uint32_t)want < g_rmode_count) {
						last_prop_mode = want;
						LOGI("debug.dxr.mode=%d -> requesting rendering mode", want);
						g_pfnReqMode(g_session, (uint32_t)want);
					}
				}
			}
			// Double-tap recenter: camera pan/dolly back to 0, zoom back to the
			// auto-fit scale. Pure atomic stores — instant, no Vulkan work.
			if (g_reset_view_request.exchange(false, std::memory_order_relaxed)) {
				g_cam_pan_x.store(0.0f, std::memory_order_relaxed);
				g_cam_dolly_z.store(0.0f, std::memory_order_relaxed);
				g_scene_scale.store(g_fit_scale, std::memory_order_relaxed);
			}
			// Drive frames from READY (not SYNCHRONIZED+): a CTS-compliant
			// runtime only advances READY->SYNCHRONIZED on the first
			// xrBeginFrame, so gating on SYNCHRONIZED+ deadlocks at READY ->
			// black (David's #507). render_frame honors shouldRender.
			//
			// #558 overlay mode: once bring-up is done (g_instance up), keep
			// rendering even with no NativeActivity window — the avatar has
			// stepped to the background so the launcher is live, and in OOP we
			// render to the IPC swapchain, not our own window. The keep-alive
			// foreground service prevents the process from being frozen.
			const bool have_window = (app->window != nullptr);
			const bool overlay_bg = g_overlay_mode && g_instance != XR_NULL_HANDLE;
			if (g_session_running && (have_window || overlay_bg)) {
				render_frame();
			}
		}
	}
}
