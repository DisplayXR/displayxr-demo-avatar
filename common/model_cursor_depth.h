// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The avatar's adoption of the depth-aware cursor (XR_DXR_cursor_depth).
 *
 * The runtime DRAWS the cursor (ADR-046 Amendment 2): an arrow lifted just in
 * front of whatever pops out of the glass under the pointer, and nothing at
 * all over content on or behind the glass (the OS cursor stays the cursor
 * there). The app owns only the depth under the pointer. displayxr-common's
 * `dxr::CursorDepth` does the bookkeeping; this header is the part every leg
 * of THIS demo shares on top of it.
 *
 * PROVENANCE: this is displayxr-demo-modelviewer's common/model_cursor_depth.h
 * (the depth-cursor pilot, modelviewer PR #171), re-namespaced, plus ONE
 * avatar-specific addition: `setAppViews` (see below). Keep the shared part in
 * step with the pilot; a later move into displayxr-common would dedupe it.
 *
 *   - the depth source: `ModelRenderer::raycast` (per-cluster BVH over the
 *     loaded character, following its animated pose — model_raycast.h). The
 *     character is the only 3D content here: the speech bubble is a flat
 *     Local2D layer ON the glass, there is no skybox in the transparent
 *     default (and in the opaque debug mode it is at infinity), and no 3D UI.
 *     In the transparent default nothing behind the glass is drawn at all, so
 *     whatever the ray cast hits and the runtime then lifts the cursor over is
 *     exactly what pops out; hits behind the glass leave the OS cursor alone
 *     (the runtime's "never behind the glass" rule).
 *   - `DISPLAYXR_CURSOR_DEPTH=0` opts out (the extension is then not enabled
 *     at all, so the runtime does no cursor work), as in the runtime's cube apps.
 *   - `DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the pointer (canvas UV, origin
 *     top-left) for headless verification; the leg then ignores real motion.
 *     The canvas is whatever the leg's cursor locate frames: the tiger ZONE
 *     (bottom 75 %) on Windows/Linux, the whole window on macOS.
 *   - a throttled cost line: the ray cast's wall time per frame.
 *
 * Two depth-source shapes, by how the leg frames its views:
 *
 *   1. RIG legs (Windows, Linux): the zone-scoped locate returns render-ready
 *      views in the plain model world (XR_DXR_view_rig + setPlainViewConvention),
 *      so the locate space IS the model world and `setup()` hands dxr::CursorDepth
 *      the plain ray cast — the pilot's path, unchanged.
 *   2. APP-KOOIMA leg (macOS): the app computes its own Kooima views
 *      (display3d_compute_views, Y-mirrored render frame, bottom-75 % canvas)
 *      from the RAW display-space eyes the runtime locates. The locate space is
 *      then physical display space, not the model world, so a ray through the
 *      runtime's views would miss the character entirely. `setupAppKooima()`
 *      installs a hit test that (a) casts the footprint through the app's OWN
 *      outer views into the model world, and (b) hands the runtime the
 *      display-space point that the runtime's outer views see at the SAME two
 *      canvas positions the app drew the hit at (the closest approach of the
 *      two runtime rays). That preserves the on-screen disparity exactly,
 *      whatever the app's ipd/parallax/perspective/m2v tunables are.
 *      `setAppViews()` feeds it the outer views every frame.
 *
 * Per leg (see macos/main.mm, linux/main.cpp, windows/main.cpp):
 *
 *     // instance: enable XR_DXR_cursor_depth if present && CursorDepthWanted()
 *     avatar::CursorDepthAdoption cursorAdopt;
 *     cursorAdopt.setup(cursor, g_modelRenderer, specVersionOr0);   // or setupAppKooima
 *     // per frame, around the cursor's xrLocateViews:
 *     cursor.chain(locateInfo, viewState);
 *     xrLocateViews(...);
 *     cursor.afterLocate(views, activeViewCount);
 *     if (cursorAdopt.endFrame(cursor)) LOG_INFO("%s", cursorAdopt.summary());
 */

#pragma once

#include "cursor_depth.h"   // displayxr-common v2.29.0+
#include "model_renderer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace avatar {

//! False when DISPLAYXR_CURSOR_DEPTH=0: do not enable the extension.
inline bool CursorDepthWanted() {
    const char* e = std::getenv("DISPLAYXR_CURSOR_DEPTH");
    return !(e != nullptr && std::strcmp(e, "0") == 0);
}

//! DISPLAYXR_CURSOR_DEPTH_UV=u,v, parsed. False when unset/malformed.
inline bool CursorDepthScriptedUV(float& u, float& v) {
    const char* uv = std::getenv("DISPLAYXR_CURSOR_DEPTH_UV");
    return uv != nullptr && std::sscanf(uv, "%f,%f", &u, &v) == 2;
}

class CursorDepthAdoption {
public:
    //! enable(specVersion) (0 = extension not enabled: the helper stays inert),
    //! the model ray cast, and the scripted pointer. The window adapter stays
    //! with the leg (it is the only platform-specific part). RIG legs.
    void setup(dxr::CursorDepth& cursor, const ModelRenderer& renderer, uint32_t specVersion) {
        cursor.enable(specVersion);
        const ModelRenderer* r = &renderer;
        Stats* st = &stats_;
        cursor.setRayCast([r, st](const dxr::CursorRay& ray) -> std::optional<float> {
            const auto t0 = std::chrono::steady_clock::now();
            const float o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
            const float d[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
            float t = 0.0f;
            const bool hit = r->raycast(o, d, t);
            st->frameUs += std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - t0).count();
            st->frameRays++;
            return hit ? std::optional<float>(t) : std::nullopt;
        });
        setupScripted(cursor);
    }

    //! The APP-KOOIMA leg (macOS): same as setup(), but the depth source is the
    //! two-frame hit test described in the file header. Feed it with
    //! setAppViews() every frame the app renders 3D (clearAppViews() otherwise).
    void setupAppKooima(dxr::CursorDepth& cursor, const ModelRenderer& renderer, uint32_t specVersion) {
        cursor.enable(specVersion);
        const ModelRenderer* r = &renderer;
        const dxr::CursorDepth* c = &cursor;
        cursor.setHitTest([this, r, c](const XrView* rv, uint32_t n, XrVector2f uv,
                                       const dxr::CursorFootprintRays&) -> std::optional<XrVector3f> {
            const auto t0 = std::chrono::steady_clock::now();
            std::optional<XrVector3f> out = appKooimaHit(*r, c->cursorHeight(), rv, n, uv);
            stats_.frameUs += std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - t0).count();
            return out;
        });
        setupScripted(cursor);
    }

    /*!
     * APP-KOOIMA leg: this frame's OUTER app views (the first and the last
     * rendered view — they pair with the runtime's views[0] and
     * views[activeViewCount-1]). view/proj are exactly what renderEye gets
     * (column-major; proj with [0,1] clip z, +Y-up NDC). `legacyMirror` = the
     * renderer's default view convention (world Y flipped at the view stage,
     * no raster flip); false = setPlainViewConvention(true). The app canvas is
     * window rows [canvasTop, 1] (v down): the avatar's bottom sub-rect.
     */
    void setAppViews(const float view0[16], const float proj0[16], const float view1[16],
                     const float proj1[16], bool legacyMirror, float canvasTop) {
        const float* v[2] = {view0, view1};
        const float* p[2] = {proj0, proj1};
        for (int k = 0; k < 2; ++k) {
            float vm[16];
            std::memcpy(vm, v[k], sizeof(vm));
            if (legacyMirror) for (int i = 4; i < 8; ++i) vm[i] = -vm[i];   // V * diag(1,-1,1,1)
            mul(p[k], vm, app_.vp[k]);
            app_.ok[k] = invert(app_.vp[k], app_.ivp[k]);
        }
        app_.plain = !legacyMirror;
        app_.canvasTop = canvasTop;
        app_.valid = app_.ok[0] && app_.ok[1];
    }
    void clearAppViews() { app_.valid = false; }

    //! The pointer is scripted (DISPLAYXR_CURSOR_DEPTH_UV): ignore real motion.
    bool scripted() const { return scripted_; }
    //! The scripted UV (valid while scripted()).
    float scriptedU() const { return su_; }
    float scriptedV() const { return sv_; }

    //! Call once per frame after afterLocate(). True when a cost summary is
    //! ready (every 600 frames that ran the ray cast); read it with summary().
    bool endFrame(const dxr::CursorDepth& cursor) {
        if (stats_.frameRays > 0) {
            stats_.frames++;
            stats_.sumUs += stats_.frameUs;
            stats_.sumRays += stats_.frameRays;
            if (stats_.frameUs > stats_.maxUs) stats_.maxUs = stats_.frameUs;
            if (cursor.hint().hasContent) stats_.hitFrames++;
            if (cursor.drawState().willDraw) stats_.drawFrames++;
        }
        stats_.frameUs = 0.0;
        stats_.frameRays = 0;
        if (stats_.frames < kSummaryFrames) return false;
        std::snprintf(summary_, sizeof(summary_),
                      "cursor depth: ray cast %.1f us/frame mean, %.1f us max, %.1f rays/frame "
                      "over %u frames (hit %u, runtime drew %u)",
                      stats_.sumUs / stats_.frames, stats_.maxUs, (double)stats_.sumRays / stats_.frames,
                      stats_.frames, stats_.hitFrames, stats_.drawFrames);
        if (scripted_) {
            const XrCursorDepthPlacementDXR& pl = cursor.placement();
            const size_t n = std::strlen(summary_);
            std::snprintf(summary_ + n, sizeof(summary_) - n,
                          "; placement active=%d disparity=%.3f target=%.3f pos=(%.4f, %.4f, %.4f) h=%.4f",
                          (int)pl.isActive, pl.disparity, pl.targetDisparity, pl.pose.position.x,
                          pl.pose.position.y, pl.pose.position.z, pl.height);
        }
        stats_ = Stats{};
        return true;
    }
    const char* summary() const { return summary_; }

private:
    void setupScripted(dxr::CursorDepth& cursor) {
        scripted_ = false;
        if (CursorDepthScriptedUV(su_, sv_)) {
            cursor.setPointerUV(su_, sv_);
            cursor.setWantPlacement(true);   // the placement goes into the summary
            scripted_ = true;
        }
    }

    // ---- APP-KOOIMA hit test ------------------------------------------------

    static void mul(const float a[16], const float b[16], float o[16]) {   // o = a * b (column-major)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
                o[c * 4 + r] = s;
            }
    }
    static bool invert(const float m[16], float o[16]) {   // general 4x4 (cofactors)
        double inv[16], a[16];
        for (int i = 0; i < 16; ++i) a[i] = m[i];
        inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
        inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
        inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
        inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
        inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
        inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
        inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
        inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
        inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
        inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
        inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
        inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
        inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
        inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
        inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
        inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
        const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (!(std::fabs(det) > 1e-30)) return false;
        for (int i = 0; i < 16; ++i) o[i] = (float)(inv[i] / det);
        return true;
    }
    static bool xform(const float m[16], const float p[3], float w, float out[4]) {
        for (int r = 0; r < 4; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] * w;
        return std::isfinite(out[3]) && std::fabs(out[3]) > 1e-12f;
    }
    // NDC y of a canvas row. Legacy convention: no raster flip, so Vulkan's
    // NDC -1 is the TOP row. Plain: the renderer flips at the raster stage
    // (negative-height viewport), so +1 is the top.
    float ndcY(float vCanvas) const { return app_.plain ? 1.0f - 2.0f * vCanvas : 2.0f * vCanvas - 1.0f; }
    float canvasV(float ndcy) const { return app_.plain ? (1.0f - ndcy) * 0.5f : (ndcy + 1.0f) * 0.5f; }

    std::optional<XrVector3f> appKooimaHit(const ModelRenderer& r, float heightFrac, const XrView* rv,
                                           uint32_t n, XrVector2f uv) {
        if (!app_.valid || rv == nullptr || n < 2) return std::nullopt;
        float aspect = dxr::cursorCanvasAspect(rv[0]);
        XrVector2f pts[dxr::kCursorFootprintMaxPoints];
        const uint32_t np = dxr::cursorFootprintUV(uv, heightFrac, aspect, pts);
        const float top = app_.canvasTop, span = 1.0f - app_.canvasTop;
        if (!(span > 0.0f)) return std::nullopt;

        // (a) Footprint rays through the APP's outer views, in the model world.
        bool have = false;
        float best[3] = {0, 0, 0}, bestZ = 0.0f;
        for (uint32_t i = 0; i < np; ++i) {
            const float uc = pts[i].x, vc = (pts[i].y - top) / span;
            if (uc < 0.0f || uc > 1.0f || vc < 0.0f || vc > 1.0f) continue;   // off the 3D canvas (bubble band)
            const float nx = 2.0f * uc - 1.0f, ny = ndcY(vc);
            for (int k = 0; k < 2; ++k) {
                const float pn[3] = {nx, ny, 0.0f}, pf[3] = {nx, ny, 1.0f};
                float a[4], b[4];
                if (!xform(app_.ivp[k], pn, 1.0f, a) || !xform(app_.ivp[k], pf, 1.0f, b)) continue;
                const float o[3] = {a[0] / a[3], a[1] / a[3], a[2] / a[3]};
                const float d[3] = {b[0] / b[3] - o[0], b[1] / b[3] - o[1], b[2] / b[3] - o[2]};
                float t = 0.0f;
                stats_.frameRays++;
                if (!r.raycast(o, d, t) || !(t >= 0.0f)) continue;
                const float h[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
                float c[4];
                if (!xform(app_.vp[0], h, 1.0f, c)) continue;
                const float z = c[2] / c[3];   // [0,1] clip z: smaller = nearer the viewer
                if (!have || z < bestZ) { have = true; bestZ = z; std::memcpy(best, h, sizeof(best)); }
            }
        }
        if (!have) return std::nullopt;

        // (b) Where the app DRAWS that point in each outer view (window UV) ->
        //     the display-space point the RUNTIME's outer views see there.
        dxr::CursorRay ray[2];
        for (int k = 0; k < 2; ++k) {
            float c[4];
            if (!xform(app_.vp[k], best, 1.0f, c) || !(c[3] > 0.0f)) return std::nullopt;
            const float u = (c[0] / c[3] + 1.0f) * 0.5f;
            const float v = top + canvasV(c[1] / c[3]) * span;
            ray[k] = dxr::cursorViewRay(rv[k == 0 ? 0 : n - 1], u, v);
        }
        // Closest approach of the two rays; the midpoint is the point.
        const XrVector3f& o0 = ray[0].origin; const XrVector3f& d0 = ray[0].dir;
        const XrVector3f& o1 = ray[1].origin; const XrVector3f& d1 = ray[1].dir;
        const XrVector3f w = {o0.x - o1.x, o0.y - o1.y, o0.z - o1.z};
        const float a = d0.x * d0.x + d0.y * d0.y + d0.z * d0.z;
        const float b = d0.x * d1.x + d0.y * d1.y + d0.z * d1.z;
        const float cc = d1.x * d1.x + d1.y * d1.y + d1.z * d1.z;
        const float d = d0.x * w.x + d0.y * w.y + d0.z * w.z;
        const float e = d1.x * w.x + d1.y * w.y + d1.z * w.z;
        const float den = a * cc - b * b;
        if (!(std::fabs(den) > 1e-12f)) return std::nullopt;   // parallel: no depth to recover
        const float s = (b * e - cc * d) / den, t = (a * e - b * d) / den;
        if (!(s > 0.0f) || !(t > 0.0f)) return std::nullopt;
        return XrVector3f{0.5f * (o0.x + d0.x * s + o1.x + d1.x * t), 0.5f * (o0.y + d0.y * s + o1.y + d1.y * t),
                          0.5f * (o0.z + d0.z * s + o1.z + d1.z * t)};
    }

    static constexpr uint32_t kSummaryFrames = 600;
    struct Stats {
        double frameUs = 0.0;
        uint32_t frameRays = 0;
        uint32_t frames = 0, hitFrames = 0, drawFrames = 0;
        uint64_t sumRays = 0;
        double sumUs = 0.0, maxUs = 0.0;
    };
    struct AppViews {
        float vp[2][16] = {};    // model world -> clip, per outer view (mirror folded in)
        float ivp[2][16] = {};
        bool ok[2] = {false, false};
        bool plain = false;
        float canvasTop = 0.0f;
        bool valid = false;
    };
    Stats stats_;
    AppViews app_;
    bool scripted_ = false;
    float su_ = 0.0f, sv_ = 0.0f;
    char summary_[512] = {};
};

} // namespace avatar
