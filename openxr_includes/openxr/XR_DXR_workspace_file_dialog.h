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
 * @brief  Header for XR_DXR_workspace_file_dialog extension (Tier 1 spatial file picker).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Async spatial-native file picker. The app calls xrRequestFilePickerDXR and
 * receives an XrEventDataFilePickerCompleteDXR through xrPollEvent when the
 * user picks (or cancels). The picker itself is a peer workspace window
 * (its own OpenXR handle app) spawned by the active workspace controller —
 * NOT a layer inside the requester's window.
 *
 * Async / event-based on purpose: a blocking call would deadlock single-
 * threaded render loops and stall xrWaitFrame.
 *
 * The extension is workspace-scoped: an instance must enable
 * XR_DXR_spatial_workspace and the session must be the active workspace
 * (xrActivateSpatialWorkspaceDXR) before xrRequestFilePickerDXR returns
 * XR_SUCCESS. Outside a workspace it returns XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * Fallback to Tier 0: if no workspace controller is registered, or the
 * active controller does not advertise file-dialog support (registry value
 * `SupportsFileDialog=1` under its WorkspaceControllers\<id> key), the call
 * returns XR_FILE_PICKER_FALLBACK_TIER0_DXR immediately. Apps should then
 * call GetOpenFileName / IFileOpenDialog themselves; the Tier 0 CBT hook
 * (always installed under DISPLAYXR_WORKSPACE_SESSION=1) handles z-order
 * and focus restoration onto a visible offscreen owner HWND.
 */
#ifndef XR_DXR_WORKSPACE_FILE_DIALOG_H
#define XR_DXR_WORKSPACE_FILE_DIALOG_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_workspace_file_dialog 1
#define XR_DXR_workspace_file_dialog_SPEC_VERSION 1
#define XR_DXR_WORKSPACE_FILE_DIALOG_EXTENSION_NAME "XR_DXR_workspace_file_dialog"

// Provisional XrStructureType values. The 1004999120..121 range is reserved
// for this extension. Reconciles with the Khronos registry before any
// spec-freeze attempt.
#define XR_TYPE_FILE_PICKER_INFO_DXR                ((XrStructureType)1004999120)
#define XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_DXR ((XrStructureType)1004999121)

/*!
 * @brief Success-class XrResult returned when no Tier 1 picker is available.
 *
 * Cast as XrResult so callers can compare against the function return value
 * directly. Positive (non-error) per OpenXR's result-code convention.
 */
#define XR_FILE_PICKER_FALLBACK_TIER0_DXR ((XrResult)1004999122)

/*!
 * @brief Maximum path length carried in the completion event (UTF-8 bytes).
 *
 * Sized to Windows `MAX_PATH` (260) rounded down to a power-of-two-ish
 * value so the encompassing IPC request message stays comfortably inside
 * the runtime's per-message wire budget. Apps that need long-path
 * support (`\\?\…` style) should call the Tier 0 path directly; the
 * completion event returns `XR_FILE_PICKER_RESULT_INVALID_PATH_DXR` if
 * the user picks a path that does not fit in this buffer.
 */
#define XR_MAX_FILE_PICKER_PATH_LENGTH_DXR 256

/*!
 * @brief Maximum length of a single filter description / extension list
 * field in XrFilePickerInfoDXR.
 */
#define XR_MAX_FILE_PICKER_FILTER_LENGTH_DXR 64

/*!
 * @brief Maximum length of the optional picker title.
 */
#define XR_MAX_FILE_PICKER_TITLE_LENGTH_DXR 128

/*!
 * @brief Maximum number of filter entries the picker UI displays.
 */
#define XR_MAX_FILE_PICKER_FILTERS_DXR 4

/*!
 * @brief Async-request handle returned by xrRequestFilePickerDXR.
 *
 * Opaque monotonic ID. Apps correlate completion events by matching the
 * `requestId` field of XrEventDataFilePickerCompleteDXR against the value
 * the runtime wrote here. The runtime drops outstanding requests on
 * session destroy; late completions for a destroyed session are no-ops.
 */
typedef uint64_t XrAsyncRequestIdDXR;

#define XR_NULL_ASYNC_REQUEST_ID_DXR ((XrAsyncRequestIdDXR)0)

/*!
 * @brief Picker mode.
 */
typedef enum XrFilePickerModeDXR {
    XR_FILE_PICKER_MODE_OPEN_DXR   = 0,
    XR_FILE_PICKER_MODE_SAVE_DXR   = 1,
    XR_FILE_PICKER_MODE_FOLDER_DXR = 2,
    XR_FILE_PICKER_MODE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrFilePickerModeDXR;

/*!
 * @brief Flags controlling picker UI behavior.
 *
 * MULTI_SELECT_BIT is reserved for spec_version 2. spec_version 1 picker
 * implementations may return XR_ERROR_FEATURE_UNSUPPORTED if the flag is
 * set.
 */
typedef XrFlags64 XrFilePickerFlagsDXR;
static const XrFilePickerFlagsDXR XR_FILE_PICKER_FLAG_NONE_DXR             = 0;
static const XrFilePickerFlagsDXR XR_FILE_PICKER_FLAG_MULTI_SELECT_BIT_DXR = 0x00000001;

/*!
 * @brief One filter row in the picker's dropdown.
 *
 * `extensions` is a semicolon-delimited list of patterns, e.g.
 * `"*.png;*.jpg;*.jpeg"`. Empty = match all.
 */
typedef struct XrFilePickerFilterDXR {
    char description[XR_MAX_FILE_PICKER_FILTER_LENGTH_DXR]; //!< User-visible label, e.g. "Images"
    char extensions[XR_MAX_FILE_PICKER_FILTER_LENGTH_DXR];  //!< Semicolon-delimited patterns
} XrFilePickerFilterDXR;

/*!
 * @brief Request parameters for xrRequestFilePickerDXR.
 *
 * The runtime forwards this struct to the active workspace controller
 * over IPC. All character fields are NUL-terminated UTF-8. The IPC
 * codegen does not follow `next` pointer chains — this struct must be
 * a flat copyable value.
 */
typedef struct XrFilePickerInfoDXR {
    XrStructureType            type;       //!< Must be XR_TYPE_FILE_PICKER_INFO_DXR
    const void* XR_MAY_ALIAS   next;       //!< Reserved; must be NULL in spec_version 1
    XrFilePickerModeDXR        mode;       //!< Open / Save / Folder
    XrFilePickerFlagsDXR       flags;      //!< See XR_FILE_PICKER_FLAG_*
    char                       title[XR_MAX_FILE_PICKER_TITLE_LENGTH_DXR]; //!< Window title; empty = picker chooses
    char                       defaultPath[XR_MAX_FILE_PICKER_PATH_LENGTH_DXR]; //!< Starting directory; empty = picker chooses
    uint32_t                   filterCount; //!< Number of valid entries in filters[]
    XrFilePickerFilterDXR      filters[XR_MAX_FILE_PICKER_FILTERS_DXR];
} XrFilePickerInfoDXR;

/*!
 * @brief Begin an async file-picker request.
 *
 * Returns immediately. The runtime allocates a monotonic request ID, hands
 * the request off to the active workspace controller, and queues an
 * XrEventDataFilePickerCompleteDXR on the requesting session's event
 * stream when the picker completes.
 *
 * Apps poll xrPollEvent on the same session to receive the completion.
 *
 * @param session    A valid XrSession handle that has activated a
 *                   spatial workspace via xrActivateSpatialWorkspaceDXR.
 * @param info       Picker parameters. Must not be NULL.
 * @param requestId  Output: monotonic ID for correlating the completion
 *                   event. Must not be NULL. Never zero on success.
 * @return XR_SUCCESS — request accepted; event will follow.
 *         XR_FILE_PICKER_FALLBACK_TIER0_DXR — no spatial picker available;
 *         the app should fall back to a flat OS dialog (Tier 0 handles
 *         z-order and focus restoration automatically). No completion
 *         event will be queued; *requestId is set to
 *         XR_NULL_ASYNC_REQUEST_ID_DXR.
 *         XR_ERROR_FEATURE_UNSUPPORTED — session is not running under a
 *         workspace.
 *         XR_ERROR_VALIDATION_FAILURE — info->type is wrong, mode is
 *         invalid, or the picker is being called from the picker
 *         process itself (recursion guard).
 *         XR_ERROR_HANDLE_INVALID — session is invalid.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestFilePickerDXR)(
    XrSession                       session,
    const XrFilePickerInfoDXR*      info,
    XrAsyncRequestIdDXR*            requestId);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestFilePickerDXR(
    XrSession                       session,
    const XrFilePickerInfoDXR*      info,
    XrAsyncRequestIdDXR*            requestId);
#endif

/*!
 * @brief Picker completion result codes carried in the event.
 *
 * Distinct from `XrResult` to avoid stepping on the Khronos result-code
 * range; encoded as int32_t in the event.
 */
typedef enum XrFilePickerResultDXR {
    XR_FILE_PICKER_RESULT_SUCCESS_DXR     = 0,  //!< User picked. `path` is valid.
    XR_FILE_PICKER_RESULT_CANCELLED_DXR   = 1,  //!< User cancelled. `path` is empty.
    XR_FILE_PICKER_RESULT_PICKER_FAILED_DXR = 2, //!< Picker exited abnormally / crashed.
    XR_FILE_PICKER_RESULT_INVALID_PATH_DXR = 3, //!< User picked a path that did not fit in the buffer.
    XR_FILE_PICKER_RESULT_MAX_ENUM_DXR    = 0x7FFFFFFF
} XrFilePickerResultDXR;

/*!
 * @brief Completion event delivered via xrPollEvent.
 *
 * The runtime routes this event to the session whose xrRequestFilePickerDXR
 * call produced the matching `requestId`. If the requesting session is
 * destroyed before the picker completes, the event is dropped silently.
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataFilePickerCompleteDXR {
    XrStructureType             type;       //!< Must be XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_DXR
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrAsyncRequestIdDXR         requestId;
    XrFilePickerResultDXR       result;
    char                        path[XR_MAX_FILE_PICKER_PATH_LENGTH_DXR]; //!< NUL-terminated UTF-8; empty on cancel/failure
} XrEventDataFilePickerCompleteDXR;

// ---- Controller-side surface ----
//
// Workspace controllers drain XR_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST_DXR
// from xrEnumerateWorkspaceInputEventsDXR, fetch the full request via
// xrGetFilePickerRequestDXR, spawn (or otherwise drive) their picker UI, and
// deliver the result through xrCompleteFilePickerDXR. Both functions require
// the calling session to hold the active workspace role
// (xrActivateSpatialWorkspaceDXR). A non-controller caller receives
// XR_ERROR_FEATURE_UNSUPPORTED.

/*!
 * @brief Fetch the full picker request that a workspace client posted via
 * xrRequestFilePickerDXR.
 *
 * Called by the workspace controller in response to an
 * XR_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST_DXR event. The runtime
 * returns the requesting client's id and the picker info struct so the
 * controller can spawn / drive a picker UI.
 *
 * @param session    Workspace-controller session.
 * @param requestId  Value carried by the request event.
 * @param outClientId  Output: the requesting workspace client.
 * @param outInfo     Output: filled with the requester's
 *                    XrFilePickerInfoDXR-equivalent. `type` and `next`
 *                    are reset by the runtime so the caller can pass
 *                    the buffer along to its picker without further
 *                    re-initialisation.
 * @return XR_SUCCESS on hit;
 *         XR_ERROR_VALIDATION_FAILURE if requestId is 0 or no pending
 *         entry matches (already completed, or the requester
 *         disconnected);
 *         XR_ERROR_FEATURE_UNSUPPORTED if the calling session is not
 *         the active workspace controller.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetFilePickerRequestDXR)(
    XrSession              session,
    XrAsyncRequestIdDXR    requestId,
    uint32_t              *outClientId,    //!< XrWorkspaceClientId (header-independence: plain uint32_t)
    XrFilePickerInfoDXR   *outInfo);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrGetFilePickerRequestDXR(
    XrSession              session,
    XrAsyncRequestIdDXR    requestId,
    uint32_t              *outClientId,
    XrFilePickerInfoDXR   *outInfo);
#endif

/*!
 * @brief Deliver a picker result back to the requesting client.
 *
 * The runtime translates this into an XrEventDataFilePickerCompleteDXR
 * pushed onto the requester's session event queue. Late results (i.e.,
 * the requester disconnected before the controller responded) are
 * dropped silently and logged once.
 *
 * @param session    Workspace-controller session.
 * @param requestId  Value carried by the request event.
 * @param result     XR_FILE_PICKER_RESULT_*. SUCCESS_EXT must be paired
 *                   with a non-empty path; the runtime overrides
 *                   `path` to an empty string for non-SUCCESS results.
 * @param path       NUL-terminated UTF-8; ignored when @p result is not
 *                   SUCCESS_EXT. May be NULL or empty in that case.
 * @return XR_SUCCESS on a successful deliver-or-late-result-drop;
 *         XR_ERROR_VALIDATION_FAILURE if requestId is 0;
 *         XR_ERROR_FEATURE_UNSUPPORTED if the calling session is not
 *         the active workspace controller;
 *         XR_ERROR_PATH_FORMAT_INVALID if @p path exceeds the runtime's
 *         wire budget (see XR_MAX_FILE_PICKER_PATH_LENGTH_DXR).
 */
typedef XrResult (XRAPI_PTR *PFN_xrCompleteFilePickerDXR)(
    XrSession                  session,
    XrAsyncRequestIdDXR        requestId,
    XrFilePickerResultDXR      result,
    const char                *path);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrCompleteFilePickerDXR(
    XrSession              session,
    XrAsyncRequestIdDXR    requestId,
    XrFilePickerResultDXR  result,
    const char            *path);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_WORKSPACE_FILE_DIALOG_H
