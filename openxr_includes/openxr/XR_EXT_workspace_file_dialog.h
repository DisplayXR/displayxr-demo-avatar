// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_workspace_file_dialog extension (Tier 1 spatial file picker).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Async spatial-native file picker. The app calls xrRequestFilePickerEXT and
 * receives an XrEventDataFilePickerCompleteEXT through xrPollEvent when the
 * user picks (or cancels). The picker itself is a peer workspace window
 * (its own OpenXR handle app) spawned by the active workspace controller —
 * NOT a layer inside the requester's window.
 *
 * Async / event-based on purpose: a blocking call would deadlock single-
 * threaded render loops and stall xrWaitFrame.
 *
 * The extension is workspace-scoped: an instance must enable
 * XR_EXT_spatial_workspace and the session must be the active workspace
 * (xrActivateSpatialWorkspaceEXT) before xrRequestFilePickerEXT returns
 * XR_SUCCESS. Outside a workspace it returns XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * Fallback to Tier 0: if no workspace controller is registered, or the
 * active controller does not advertise file-dialog support (registry value
 * `SupportsFileDialog=1` under its WorkspaceControllers\<id> key), the call
 * returns XR_FILE_PICKER_FALLBACK_TIER0_EXT immediately. Apps should then
 * call GetOpenFileName / IFileOpenDialog themselves; the Tier 0 CBT hook
 * (always installed under DISPLAYXR_WORKSPACE_SESSION=1) handles z-order
 * and focus restoration onto a visible offscreen owner HWND.
 */
#ifndef XR_EXT_WORKSPACE_FILE_DIALOG_H
#define XR_EXT_WORKSPACE_FILE_DIALOG_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_workspace_file_dialog 1
#define XR_EXT_workspace_file_dialog_SPEC_VERSION 1
#define XR_EXT_WORKSPACE_FILE_DIALOG_EXTENSION_NAME "XR_EXT_workspace_file_dialog"

// Provisional XrStructureType values. The 1000999120..121 range is reserved
// for this extension. Reconciles with the Khronos registry before any
// spec-freeze attempt.
#define XR_TYPE_FILE_PICKER_INFO_EXT                ((XrStructureType)1000999120)
#define XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_EXT ((XrStructureType)1000999121)

/*!
 * @brief Success-class XrResult returned when no Tier 1 picker is available.
 *
 * Cast as XrResult so callers can compare against the function return value
 * directly. Positive (non-error) per OpenXR's result-code convention.
 */
#define XR_FILE_PICKER_FALLBACK_TIER0_EXT ((XrResult)1000999122)

/*!
 * @brief Maximum path length carried in the completion event (UTF-8 bytes).
 *
 * Sized to Windows `MAX_PATH` (260) rounded down to a power-of-two-ish
 * value so the encompassing IPC request message stays comfortably inside
 * the runtime's per-message wire budget. Apps that need long-path
 * support (`\\?\…` style) should call the Tier 0 path directly; the
 * completion event returns `XR_FILE_PICKER_RESULT_INVALID_PATH_EXT` if
 * the user picks a path that does not fit in this buffer.
 */
#define XR_MAX_FILE_PICKER_PATH_LENGTH_EXT 256

/*!
 * @brief Maximum length of a single filter description / extension list
 * field in XrFilePickerInfoEXT.
 */
#define XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT 64

/*!
 * @brief Maximum length of the optional picker title.
 */
#define XR_MAX_FILE_PICKER_TITLE_LENGTH_EXT 128

/*!
 * @brief Maximum number of filter entries the picker UI displays.
 */
#define XR_MAX_FILE_PICKER_FILTERS_EXT 4

/*!
 * @brief Async-request handle returned by xrRequestFilePickerEXT.
 *
 * Opaque monotonic ID. Apps correlate completion events by matching the
 * `requestId` field of XrEventDataFilePickerCompleteEXT against the value
 * the runtime wrote here. The runtime drops outstanding requests on
 * session destroy; late completions for a destroyed session are no-ops.
 */
typedef uint64_t XrAsyncRequestIdEXT;

#define XR_NULL_ASYNC_REQUEST_ID_EXT ((XrAsyncRequestIdEXT)0)

/*!
 * @brief Picker mode.
 */
typedef enum XrFilePickerModeEXT {
    XR_FILE_PICKER_MODE_OPEN_EXT   = 0,
    XR_FILE_PICKER_MODE_SAVE_EXT   = 1,
    XR_FILE_PICKER_MODE_FOLDER_EXT = 2,
    XR_FILE_PICKER_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrFilePickerModeEXT;

/*!
 * @brief Flags controlling picker UI behavior.
 *
 * MULTI_SELECT_BIT is reserved for spec_version 2. spec_version 1 picker
 * implementations may return XR_ERROR_FEATURE_UNSUPPORTED if the flag is
 * set.
 */
typedef XrFlags64 XrFilePickerFlagsEXT;
static const XrFilePickerFlagsEXT XR_FILE_PICKER_FLAG_NONE_EXT             = 0;
static const XrFilePickerFlagsEXT XR_FILE_PICKER_FLAG_MULTI_SELECT_BIT_EXT = 0x00000001;

/*!
 * @brief One filter row in the picker's dropdown.
 *
 * `extensions` is a semicolon-delimited list of patterns, e.g.
 * `"*.png;*.jpg;*.jpeg"`. Empty = match all.
 */
typedef struct XrFilePickerFilterEXT {
    char description[XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT]; //!< User-visible label, e.g. "Images"
    char extensions[XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT];  //!< Semicolon-delimited patterns
} XrFilePickerFilterEXT;

/*!
 * @brief Request parameters for xrRequestFilePickerEXT.
 *
 * The runtime forwards this struct to the active workspace controller
 * over IPC. All character fields are NUL-terminated UTF-8. The IPC
 * codegen does not follow `next` pointer chains — this struct must be
 * a flat copyable value.
 */
typedef struct XrFilePickerInfoEXT {
    XrStructureType            type;       //!< Must be XR_TYPE_FILE_PICKER_INFO_EXT
    const void* XR_MAY_ALIAS   next;       //!< Reserved; must be NULL in spec_version 1
    XrFilePickerModeEXT        mode;       //!< Open / Save / Folder
    XrFilePickerFlagsEXT       flags;      //!< See XR_FILE_PICKER_FLAG_*
    char                       title[XR_MAX_FILE_PICKER_TITLE_LENGTH_EXT]; //!< Window title; empty = picker chooses
    char                       defaultPath[XR_MAX_FILE_PICKER_PATH_LENGTH_EXT]; //!< Starting directory; empty = picker chooses
    uint32_t                   filterCount; //!< Number of valid entries in filters[]
    XrFilePickerFilterEXT      filters[XR_MAX_FILE_PICKER_FILTERS_EXT];
} XrFilePickerInfoEXT;

/*!
 * @brief Begin an async file-picker request.
 *
 * Returns immediately. The runtime allocates a monotonic request ID, hands
 * the request off to the active workspace controller, and queues an
 * XrEventDataFilePickerCompleteEXT on the requesting session's event
 * stream when the picker completes.
 *
 * Apps poll xrPollEvent on the same session to receive the completion.
 *
 * @param session    A valid XrSession handle that has activated a
 *                   spatial workspace via xrActivateSpatialWorkspaceEXT.
 * @param info       Picker parameters. Must not be NULL.
 * @param requestId  Output: monotonic ID for correlating the completion
 *                   event. Must not be NULL. Never zero on success.
 * @return XR_SUCCESS — request accepted; event will follow.
 *         XR_FILE_PICKER_FALLBACK_TIER0_EXT — no spatial picker available;
 *         the app should fall back to a flat OS dialog (Tier 0 handles
 *         z-order and focus restoration automatically). No completion
 *         event will be queued; *requestId is set to
 *         XR_NULL_ASYNC_REQUEST_ID_EXT.
 *         XR_ERROR_FEATURE_UNSUPPORTED — session is not running under a
 *         workspace.
 *         XR_ERROR_VALIDATION_FAILURE — info->type is wrong, mode is
 *         invalid, or the picker is being called from the picker
 *         process itself (recursion guard).
 *         XR_ERROR_HANDLE_INVALID — session is invalid.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestFilePickerEXT)(
    XrSession                       session,
    const XrFilePickerInfoEXT*      info,
    XrAsyncRequestIdEXT*            requestId);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestFilePickerEXT(
    XrSession                       session,
    const XrFilePickerInfoEXT*      info,
    XrAsyncRequestIdEXT*            requestId);
#endif

/*!
 * @brief Picker completion result codes carried in the event.
 *
 * Distinct from `XrResult` to avoid stepping on the Khronos result-code
 * range; encoded as int32_t in the event.
 */
typedef enum XrFilePickerResultEXT {
    XR_FILE_PICKER_RESULT_SUCCESS_EXT     = 0,  //!< User picked. `path` is valid.
    XR_FILE_PICKER_RESULT_CANCELLED_EXT   = 1,  //!< User cancelled. `path` is empty.
    XR_FILE_PICKER_RESULT_PICKER_FAILED_EXT = 2, //!< Picker exited abnormally / crashed.
    XR_FILE_PICKER_RESULT_INVALID_PATH_EXT = 3, //!< User picked a path that did not fit in the buffer.
    XR_FILE_PICKER_RESULT_MAX_ENUM_EXT    = 0x7FFFFFFF
} XrFilePickerResultEXT;

/*!
 * @brief Completion event delivered via xrPollEvent.
 *
 * The runtime routes this event to the session whose xrRequestFilePickerEXT
 * call produced the matching `requestId`. If the requesting session is
 * destroyed before the picker completes, the event is dropped silently.
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataFilePickerCompleteEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_EXT
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrAsyncRequestIdEXT         requestId;
    XrFilePickerResultEXT       result;
    char                        path[XR_MAX_FILE_PICKER_PATH_LENGTH_EXT]; //!< NUL-terminated UTF-8; empty on cancel/failure
} XrEventDataFilePickerCompleteEXT;

// ---- Controller-side surface ----
//
// Workspace controllers drain XR_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST_EXT
// from xrEnumerateWorkspaceInputEventsEXT, fetch the full request via
// xrGetFilePickerRequestEXT, spawn (or otherwise drive) their picker UI, and
// deliver the result through xrCompleteFilePickerEXT. Both functions require
// the calling session to hold the active workspace role
// (xrActivateSpatialWorkspaceEXT). A non-controller caller receives
// XR_ERROR_FEATURE_UNSUPPORTED.

/*!
 * @brief Fetch the full picker request that a workspace client posted via
 * xrRequestFilePickerEXT.
 *
 * Called by the workspace controller in response to an
 * XR_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST_EXT event. The runtime
 * returns the requesting client's id and the picker info struct so the
 * controller can spawn / drive a picker UI.
 *
 * @param session    Workspace-controller session.
 * @param requestId  Value carried by the request event.
 * @param outClientId  Output: the requesting workspace client.
 * @param outInfo     Output: filled with the requester's
 *                    XrFilePickerInfoEXT-equivalent. `type` and `next`
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
typedef XrResult (XRAPI_PTR *PFN_xrGetFilePickerRequestEXT)(
    XrSession              session,
    XrAsyncRequestIdEXT    requestId,
    uint32_t              *outClientId,    //!< XrWorkspaceClientId (header-independence: plain uint32_t)
    XrFilePickerInfoEXT   *outInfo);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrGetFilePickerRequestEXT(
    XrSession              session,
    XrAsyncRequestIdEXT    requestId,
    uint32_t              *outClientId,
    XrFilePickerInfoEXT   *outInfo);
#endif

/*!
 * @brief Deliver a picker result back to the requesting client.
 *
 * The runtime translates this into an XrEventDataFilePickerCompleteEXT
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
 *         wire budget (see XR_MAX_FILE_PICKER_PATH_LENGTH_EXT).
 */
typedef XrResult (XRAPI_PTR *PFN_xrCompleteFilePickerEXT)(
    XrSession                  session,
    XrAsyncRequestIdEXT        requestId,
    XrFilePickerResultEXT      result,
    const char                *path);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrCompleteFilePickerEXT(
    XrSession              session,
    XrAsyncRequestIdEXT    requestId,
    XrFilePickerResultEXT  result,
    const char            *path);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_WORKSPACE_FILE_DIALOG_H
