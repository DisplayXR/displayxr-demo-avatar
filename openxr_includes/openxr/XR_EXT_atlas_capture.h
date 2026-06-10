// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_atlas_capture extension
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
 * privileged cross-client workspace capture (xrCaptureWorkspaceFrameEXT in
 * XR_EXT_spatial_workspace) stays separate; the two share the runtime readback
 * core. Full design: docs/roadmap/unified-atlas-capture.md (W6 of issue #396).
 */
#ifndef XR_EXT_ATLAS_CAPTURE_H
#define XR_EXT_ATLAS_CAPTURE_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_atlas_capture 1
// SPEC_VERSION 2: the runtime appends "_atlas_<viewCount>_<cols>x<rows>.png"
// (was a flat "_atlas.png" in v1), and the encoded PNG is always opaque
// (alpha forced to 255). See issue #425.
#define XR_EXT_atlas_capture_SPEC_VERSION 2
#define XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME "XR_EXT_atlas_capture"

// Reserved 1000999xxx range, next free slot after the workspace block
// (1000999100..110). Final values reconcile with the Khronos registry before
// spec freeze.
#define XR_TYPE_ATLAS_CAPTURE_INFO_EXT   ((XrStructureType)1000999120)
#define XR_TYPE_ATLAS_CAPTURE_RESULT_EXT ((XrStructureType)1000999121)

#define XR_ATLAS_CAPTURE_PATH_MAX_EXT 256

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
typedef enum XrAtlasCaptureStageEXT {
    XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT    = 0,
    XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT = 1,
    XR_ATLAS_CAPTURE_STAGE_MAX_ENUM_EXT        = 0x7FFFFFFF
} XrAtlasCaptureStageEXT;

/*!
 * @brief Request struct for xrCaptureAtlasEXT.
 *
 * The runtime appends a layout-encoded suffix
 * "_atlas_<viewCount>_<cols>x<rows>.png" to @c pathPrefix (e.g. a 2-view 2x1
 * capture → "<pathPrefix>_atlas_2_2x1.png"), so consumers don't re-derive the
 * multi-view atlas geometry. Callers should pass a bare prefix (e.g.
 * "<stem>-<N>") and not pre-bake the layout, to avoid duplicating it. The
 * prefix is an in-struct char array (not a separately allocated string) so the
 * same struct can cross the IPC schema unchanged.
 */
typedef struct XrAtlasCaptureInfoEXT {
    XrStructureType          type;   //!< Must be XR_TYPE_ATLAS_CAPTURE_INFO_EXT
    const void* XR_MAY_ALIAS next;
    XrAtlasCaptureStageEXT   stage;  //!< Capture stage (post-compose / projection-only)
    char                     pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_EXT];
} XrAtlasCaptureInfoEXT;

/*!
 * @brief Result returned by xrCaptureAtlasEXT.
 *
 * Same metadata block as XrWorkspaceCaptureResultEXT minus @c viewsWritten.
 * @c tileColumns / @c tileRows are populated on both paths (from the active
 * rendering mode in-process). For in-process sessions @c eyeLeftM /
 * @c eyeRightM may still be zero — eye-pose plumbing currently stops at the
 * display processor and is only surfaced on the IPC/workspace path.
 */
typedef struct XrAtlasCaptureResultEXT {
    XrStructureType    type;   //!< Must be XR_TYPE_ATLAS_CAPTURE_RESULT_EXT
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
} XrAtlasCaptureResultEXT;

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
typedef XrResult (XRAPI_PTR *PFN_xrCaptureAtlasEXT)(
    XrSession                    session,
    const XrAtlasCaptureInfoEXT *info,
    XrAtlasCaptureResultEXT     *result);

#ifndef XR_NO_PROTOTYPES
#ifdef XR_EXTENSION_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrCaptureAtlasEXT(
    XrSession                    session,
    const XrAtlasCaptureInfoEXT *info,
    XrAtlasCaptureResultEXT     *result);
#endif /* XR_EXTENSION_PROTOTYPES */
#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_ATLAS_CAPTURE_H
