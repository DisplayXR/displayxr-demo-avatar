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
 * @brief  Header for XR_DXR_atlas_capture extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * A vendor-neutral, non-privileged entry point for "snapshot the multi-view
 * atlas the runtime composes for my session to a PNG." Replaces the per-app,
 * per-graphics-API readbacks every DisplayXR consumer reimplements today: the
 * app no longer touches a staging texture — the runtime does the readback with
 * the compositor's own atlas image, at a caller-selected stage.
 *
 * Any session class (handle / texture / hosted / IPC) may call it. The
 * privileged cross-client workspace capture (xrCaptureWorkspaceFrameDXR in
 * XR_DXR_spatial_workspace) stays separate; the two share the runtime readback
 * core. Full design: docs/adr/ADR-023-unified-atlas-capture.md (W6 of issue #396).
 */
#ifndef XR_DXR_ATLAS_CAPTURE_H
#define XR_DXR_ATLAS_CAPTURE_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_atlas_capture 1
// SPEC_VERSION 3: XrStructureType values relocated 1004999120..121 ->
// 1004999170..171 (the old block collided with XR_DXR_workspace_file_dialog,
// which reserved it first). No struct/field/entry-point changes; consumers
// only need a header re-sync + rebuild.
// SPEC_VERSION 2: the runtime appends "_atlas_<viewCount>_<cols>x<rows>.png"
// (was a flat "_atlas.png" in v1), and the encoded PNG is always opaque
// (alpha forced to 255). See issue #425.
#define XR_DXR_atlas_capture_SPEC_VERSION 1
#define XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME "XR_DXR_atlas_capture"

// Reserved 1004999170..171. Final values reconcile with the Khronos registry
// before spec freeze. Allocation registry: README.md in this directory.
#define XR_TYPE_ATLAS_CAPTURE_INFO_DXR   ((XrStructureType)1004999170)
#define XR_TYPE_ATLAS_CAPTURE_RESULT_DXR ((XrStructureType)1004999171)

#define XR_ATLAS_CAPTURE_PATH_MAX_DXR 256

/*!
 * @brief Compositor stage at which to capture the atlas.
 *
 * Values match @c enum mcp_capture_mode in the runtime so no translation layer
 * is needed:
 *   POST_COMPOSE   — the atlas handed to the display processor (projection +
 *                    window-space + quad layers), end of frame.
 *   PROJECTION_ONLY — the atlas with only projection-class layers, captured at
 *                    the projection-done boundary.
 */
typedef enum XrAtlasCaptureStageDXR {
    XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_DXR    = 0,
    XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR = 1,
    XR_ATLAS_CAPTURE_STAGE_MAX_ENUM_DXR        = 0x7FFFFFFF
} XrAtlasCaptureStageDXR;

/*!
 * @brief Request struct for xrCaptureAtlasDXR.
 *
 * The runtime appends a layout-encoded suffix
 * "_atlas_<viewCount>_<cols>x<rows>.png" to @c pathPrefix (e.g. a 2-view 2x1
 * capture → "<pathPrefix>_atlas_2_2x1.png"), so consumers don't re-derive the
 * multi-view atlas geometry. Callers should pass a bare prefix (e.g.
 * "<stem>-<N>") and not pre-bake the layout, to avoid duplicating it. The
 * prefix is an in-struct char array (not a separately allocated string) so the
 * same struct can cross the IPC schema unchanged.
 */
typedef struct XrAtlasCaptureInfoDXR {
    XrStructureType          type;   //!< Must be XR_TYPE_ATLAS_CAPTURE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrAtlasCaptureStageDXR   stage;  //!< Capture stage (post-compose / projection-only)
    char                     pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_DXR];
} XrAtlasCaptureInfoDXR;

/*!
 * @brief Result returned by xrCaptureAtlasDXR.
 *
 * Same metadata block as XrWorkspaceCaptureResultDXR minus @c viewsWritten.
 * @c tileColumns / @c tileRows are populated on both paths (from the active
 * rendering mode in-process). For in-process sessions @c eyeLeftM /
 * @c eyeRightM may still be zero — eye-pose plumbing currently stops at the
 * display processor and is only surfaced on the IPC/workspace path.
 */
typedef struct XrAtlasCaptureResultDXR {
    XrStructureType    type;   //!< Must be XR_TYPE_ATLAS_CAPTURE_RESULT_DXR
    void* XR_MAY_ALIAS next;
    uint64_t           timestampNs;
    uint32_t           atlasWidth;
    uint32_t           atlasHeight;
    uint32_t           eyeWidth;
    uint32_t           eyeHeight;
    uint32_t           tileColumns;
    uint32_t           tileRows;
    float              displayWidthM;
    float              displayHeightM;
    float              eyeLeftM[3];
    float              eyeRightM[3];
} XrAtlasCaptureResultDXR;

/*!
 * @brief Capture the multi-view atlas the runtime composes for this session.
 *
 * The runtime reads the compositor's own atlas at @c info->stage and writes it
 * to a PNG named after @c info->pathPrefix (the runtime appends
 * "_atlas_<viewCount>_<cols>x<rows>.png"). The encoded PNG is always opaque
 * (alpha forced to 255 — the swapchain's alpha is undefined for display
 * output). If @c result is non-NULL it is filled with metadata describing the
 * capture.
 *
 * Timing: the call is non-blocking and latches the request; the readback runs
 * at the next composed frame (the caller's next xrEndFrame), so the PNG exists
 * shortly after this call returns rather than on return. XR_SUCCESS means the
 * capture was accepted, not that the file is already on disk. (A future async
 * variant may expose explicit completion; see docs/roadmap/3d-capture.md.)
 *
 * @param session A valid session of any class.
 * @param info    The capture request (stage + path prefix).
 * @param result  Output: capture metadata. May be NULL to capture the PNG only.
 */
typedef XrResult (XRAPI_PTR *PFN_xrCaptureAtlasDXR)(
    XrSession                    session,
    const XrAtlasCaptureInfoDXR *info,
    XrAtlasCaptureResultDXR     *result);

#ifndef XR_NO_PROTOTYPES
#ifdef XR_EXTENSION_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrCaptureAtlasDXR(
    XrSession                    session,
    const XrAtlasCaptureInfoDXR *info,
    XrAtlasCaptureResultDXR     *result);
#endif /* XR_EXTENSION_PROTOTYPES */
#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_ATLAS_CAPTURE_H
