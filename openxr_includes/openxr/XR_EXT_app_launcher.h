// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_app_launcher extension (Phase 2.B).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * The runtime renders an in-workspace tile grid; the workspace controller
 * owns the registry of tiles and pushes it via clear+add-N. This extension
 * is the public surface for that pattern. State and rendering live in the
 * runtime; what is in the registry, what to do on click, and which tiles
 * count as "running" all live in the workspace controller.
 *
 * The extension is workspace-scoped: an instance must enable
 * XR_EXT_spatial_workspace and the session must be the active workspace
 * (xrActivateSpatialWorkspaceEXT) before any of these functions return
 * XR_SUCCESS. Without a workspace they return XR_ERROR_FEATURE_UNSUPPORTED.
 */
#ifndef XR_EXT_APP_LAUNCHER_H
#define XR_EXT_APP_LAUNCHER_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_app_launcher 1
#define XR_EXT_app_launcher_SPEC_VERSION 2
#define XR_EXT_APP_LAUNCHER_EXTENSION_NAME "XR_EXT_app_launcher"

/*!
 * @brief Maximum length for short-string fields (app type, 3D-icon layout).
 */
#define XR_LAUNCHER_SHORT_STR_MAX_EXT 16

// Provisional XrStructureType value. Reconciles with the Khronos registry
// before spec freeze.
#define XR_TYPE_LAUNCHER_APP_INFO_EXT ((XrStructureType)1000999110)

/*!
 * @brief Maximum number of tiles the runtime stores.
 *
 * Matches the IPC wire-format cap (IPC_LAUNCHER_MAX_APPS = 32). Phase 2.B
 * does not change the wire format; later sub-phases may raise this if the
 * underlying IPC buffer grows. xrAddLauncherAppEXT silently drops appends
 * beyond this limit (the runtime returns XR_SUCCESS but the tile is not
 * stored — same as the existing IPC).
 */
#define XR_LAUNCHER_MAX_APPS_EXT 32

/*!
 * @brief Returned by xrPollLauncherClickEXT when no click is pending.
 */
#define XR_LAUNCHER_INVALID_APPINDEX_EXT ((int32_t)-1)

/*!
 * @brief Special action: user clicked the runtime-rendered Browse-for-app tile.
 *
 * The workspace controller dispatches this to its app-discovery flow
 * (typically: open a file picker, scan for OpenXR-compatible apps).
 */
#define XR_LAUNCHER_APPINDEX_BROWSE_EXT ((int32_t)-100)

/*!
 * @brief Base for the Remove-tile encoding.
 *
 * When the user right-click-removes a tile, xrPollLauncherClickEXT returns
 * `-(XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT + full_index)` where full_index is
 * the to-be-removed tile's slot in the controller's full registry (which
 * may be larger than the runtime's visible window when scrolled).
 *
 * Decode in the controller as:
 *   full_index = -value - XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT
 */
#define XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT ((int32_t)200)

/*!
 * @brief Special action: user requested a registry refresh (e.g., re-scan).
 *
 * The workspace controller re-discovers its app set and re-pushes via
 * xrClearLauncherAppsEXT + N × xrAddLauncherAppEXT.
 */
#define XR_LAUNCHER_APPINDEX_REFRESH_EXT ((int32_t)-300)

/*!
 * @brief One launcher tile.
 *
 * The runtime renders an icon and label. The workspace controller decides
 * what clicking means (typically: launch the named binary).
 *
 * Promotion in spec_version 2: appType / iconPath3D / iconLayout3D are now
 * exposed so a controller can drive 3D-icon rendering (the runtime composites
 * them with the indicated stereo layout). The workspace controller still
 * launches binaries on its own side; the runtime never executes binaries
 * on the controller's behalf, so no exePath is exposed here.
 *
 * appIndex is the caller-defined slot index; it must be less than
 * XR_LAUNCHER_MAX_APPS_EXT. xrPollLauncherClickEXT echoes this value
 * (or one of the special-action sentinels above) when the user clicks
 * the corresponding tile.
 */
typedef struct XrLauncherAppInfoEXT {
    XrStructureType    type;       //!< Must be XR_TYPE_LAUNCHER_APP_INFO_EXT
    void* XR_MAY_ALIAS next;
    char               name[XR_MAX_APPLICATION_NAME_SIZE]; //!< User-visible label
    char               iconPath[XR_MAX_PATH_LENGTH];       //!< 2D icon (PNG); runtime loads + caches
    int32_t            appIndex;                            //!< Tile slot, [0, XR_LAUNCHER_MAX_APPS_EXT)
    char               appType[XR_LAUNCHER_SHORT_STR_MAX_EXT];  //!< "3d" or "2d" — controls render path
    char               iconPath3D[XR_MAX_PATH_LENGTH];          //!< Optional 3D icon (PNG); empty for 2D-only tiles
    char               iconLayout3D[XR_LAUNCHER_SHORT_STR_MAX_EXT]; //!< 3D icon stereo layout, e.g. "sbs-lr"
} XrLauncherAppInfoEXT;

// ---- Tile registry ----

/*!
 * @brief Empty the launcher's tile list.
 *
 * Called by the workspace controller at the start of each registry push
 * (clear-then-add-N pattern keeps each call's payload below the IPC
 * message buffer cap and makes registry diffs trivial).
 */
typedef XrResult (XRAPI_PTR *PFN_xrClearLauncherAppsEXT)(XrSession session);

/*!
 * @brief Append one tile.
 *
 * Silently ignored (returns XR_SUCCESS) if the registry is full.
 * The workspace controller pushes a clear+add-N sequence whenever its
 * registered-app set changes.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAddLauncherAppEXT)(
    XrSession                    session,
    const XrLauncherAppInfoEXT  *info);

// ---- Visibility ----

/*!
 * @brief Show or hide the launcher tile grid.
 *
 * The runtime composites the tile grid as an overlay at zero-disparity
 * (z=0 in display space) when visible. Hidden launchers do not render
 * but the registry persists.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetLauncherVisibleEXT)(
    XrSession session, XrBool32 visible);

// ---- Click polling ----

/*!
 * @brief Poll-and-clear the most recent unconsumed tile click.
 *
 * Returns the appIndex the user clicked, or XR_LAUNCHER_INVALID_APPINDEX_EXT
 * if none. Special-action values (Browse, Remove, Refresh) are encoded as
 * negative sentinels — see the XR_LAUNCHER_APPINDEX_*_EXT defines above.
 *
 * Workspace controllers typically poll once per main-loop iteration. On a
 * hit, the controller looks up the app in its own registry (the one it
 * pushed via xrAddLauncherAppEXT) and launches the corresponding binary.
 */
typedef XrResult (XRAPI_PTR *PFN_xrPollLauncherClickEXT)(
    XrSession   session,
    int32_t    *outAppIndex);

// ---- Running indicator ----

/*!
 * @brief Set bitmask of tiles whose corresponding apps are currently running.
 *
 * The runtime draws a glow border on tiles whose bit is set, so the user
 * can see which apps are open. Bit i corresponds to the tile whose
 * appIndex was i. The workspace controller pushes this whenever its
 * computed running set changes.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetLauncherRunningTileMaskEXT)(
    XrSession session,
    uint64_t  mask);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrClearLauncherAppsEXT(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrAddLauncherAppEXT(
    XrSession                    session,
    const XrLauncherAppInfoEXT  *info);

XRAPI_ATTR XrResult XRAPI_CALL xrSetLauncherVisibleEXT(
    XrSession session,
    XrBool32  visible);

XRAPI_ATTR XrResult XRAPI_CALL xrPollLauncherClickEXT(
    XrSession session,
    int32_t  *outAppIndex);

XRAPI_ATTR XrResult XRAPI_CALL xrSetLauncherRunningTileMaskEXT(
    XrSession session,
    uint64_t  mask);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_APP_LAUNCHER_H
