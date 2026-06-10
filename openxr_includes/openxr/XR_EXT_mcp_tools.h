// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_mcp_tools extension (app-defined agent tools).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Lets an app register its own MCP (Model Context Protocol) tools —
 * "play_pause", "load_model", "run_animation" — on the per-process MCP
 * server the runtime already hosts. Agents reach them via the
 * displayxr-mcp adapter (`--target pid:N`, or namespaced as
 * `<app-id>__<tool>` through `--target workspace`).
 *
 * Dispatch is event-based on purpose: the MCP transport thread never
 * calls into app code. A tool invocation surfaces as an
 * XrEventDataMCPToolCallEXT through xrPollEvent; the app executes the
 * tool on its own frame loop (where its state is naturally consistent)
 * and answers with xrSubmitMCPToolResultEXT. The runtime fails a call
 * the app has not answered within ~5 seconds.
 *
 * Event payloads are fixed-size, so tool arguments use the OpenXR
 * two-call idiom via xrGetMCPToolCallArgsEXT.
 *
 * All functions return XR_ERROR_FEATURE_UNSUPPORTED when the MCP
 * capability is disabled (HKLM\Software\DisplayXR\Capabilities\MCP /
 * DISPLAYXR_MCP) — apps should treat that as "no agent surface" and
 * continue normally.
 *
 * Design: displayxr-runtime docs/roadmap/per-app-mcp-tools.md;
 * spec: docs/specs/extensions/XR_EXT_mcp_tools.md.
 */
#ifndef XR_EXT_MCP_TOOLS_H
#define XR_EXT_MCP_TOOLS_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_mcp_tools 1
#define XR_EXT_mcp_tools_SPEC_VERSION 1
#define XR_EXT_MCP_TOOLS_EXTENSION_NAME "XR_EXT_mcp_tools"

// Provisional XrStructureType values. The 1000999130..132 range is
// reserved for this extension. Reconciles with the Khronos registry
// before any spec-freeze attempt.
#define XR_TYPE_MCP_APP_INFO_EXT            ((XrStructureType)1000999130)
#define XR_TYPE_MCP_TOOL_INFO_EXT           ((XrStructureType)1000999131)
#define XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT ((XrStructureType)1000999132)

/*!
 * @brief Maximum app-id length (the manifest `id` slug), incl. NUL.
 *
 * Charset is `^[a-z0-9][a-z0-9-]{0,31}$` — lowercase ASCII letters,
 * digits, hyphens. Underscores are excluded by design: `__` is the
 * workspace aggregator's namespace separator.
 */
#define XR_MAX_MCP_APP_ID_SIZE_EXT 33

/*!
 * @brief Maximum bare tool-name length, incl. NUL.
 *
 * Charset is `^[a-z0-9][a-z0-9_-]{0,62}$` and the name must not
 * contain the substring `__` (reserved namespace separator).
 */
#define XR_MAX_MCP_TOOL_NAME_SIZE_EXT 64

/*!
 * @brief App identity declaration.
 *
 * `appId` MUST match the `id` field of the app's `.displayxr.json`
 * manifest (manifest spec §3.4); `scripts/check_displayxr_app.py`
 * lints the pairing. It becomes the agent-visible tool-name prefix.
 */
typedef struct XrMCPAppInfoEXT {
    XrStructureType          type; //!< Must be XR_TYPE_MCP_APP_INFO_EXT
    const void* XR_MAY_ALIAS next; //!< Reserved; must be NULL in spec_version 1
    char                     appId[XR_MAX_MCP_APP_ID_SIZE_EXT]; //!< NUL-terminated slug
} XrMCPAppInfoEXT;

/*!
 * @brief Declare the app's stable identifier. Call once per session,
 * before registering tools. May be called again to change it (the
 * runtime notifies connected agents), but treat the id as immutable
 * in practice — agents key tool names on it.
 *
 * @return XR_SUCCESS, XR_ERROR_VALIDATION_FAILURE (bad type/charset),
 *         XR_ERROR_FEATURE_UNSUPPORTED (MCP capability disabled),
 *         XR_ERROR_HANDLE_INVALID.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetMCPAppInfoEXT)(
    XrSession                session,
    const XrMCPAppInfoEXT*   info);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetMCPAppInfoEXT(
    XrSession                session,
    const XrMCPAppInfoEXT*   info);
#endif

/*!
 * @brief One tool registration.
 *
 * `description` is what the agent reads to decide when to call the
 * tool — write it like API documentation, not a label.
 * `inputSchemaJson` is a JSON Schema object describing `arguments`
 * (NULL/empty means "no arguments": `{"type":"object"}`).
 *
 * The strings are copied; the struct need not outlive the call.
 */
typedef struct XrMCPToolInfoEXT {
    XrStructureType          type; //!< Must be XR_TYPE_MCP_TOOL_INFO_EXT
    const void* XR_MAY_ALIAS next; //!< Reserved; must be NULL in spec_version 1
    const char*              name;            //!< Bare name; runtime/aggregator prefixes
    const char*              description;     //!< Agent-facing; required
    const char*              inputSchemaJson; //!< JSON Schema; optional
} XrMCPToolInfoEXT;

/*!
 * @brief Register a tool on the session. Tools registered after agents
 * connected surface immediately (the runtime broadcasts the MCP
 * `tools/list_changed` notification). All of a session's tools are
 * unregistered automatically when the session is destroyed; pending
 * calls fail with an error to the agent.
 *
 * @return XR_SUCCESS,
 *         XR_ERROR_NAME_DUPLICATED (name already registered),
 *         XR_ERROR_VALIDATION_FAILURE (bad type/name/missing description),
 *         XR_ERROR_LIMIT_REACHED (per-process tool budget exhausted),
 *         XR_ERROR_FEATURE_UNSUPPORTED, XR_ERROR_HANDLE_INVALID.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRegisterMCPToolEXT)(
    XrSession                session,
    const XrMCPToolInfoEXT*  tool);

/*!
 * @brief Unregister a tool by bare name. In-flight calls on the tool
 * fail to the agent. XR_ERROR_VALIDATION_FAILURE if the name is not
 * registered on this session.
 */
typedef XrResult (XRAPI_PTR *PFN_xrUnregisterMCPToolEXT)(
    XrSession                session,
    const char*              name);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRegisterMCPToolEXT(
    XrSession                session,
    const XrMCPToolInfoEXT*  tool);
XRAPI_ATTR XrResult XRAPI_CALL xrUnregisterMCPToolEXT(
    XrSession                session,
    const char*              name);
#endif

/*!
 * @brief Tool invocation event, delivered via xrPollEvent.
 *
 * The app fetches the JSON arguments with xrGetMCPToolCallArgsEXT
 * (`argsSize` is the required capacity incl. NUL), executes the tool,
 * and answers with xrSubmitMCPToolResultEXT. An unanswered call fails
 * to the agent after ~5 seconds; a result submitted after that is
 * silently dropped (XR_SUCCESS).
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataMCPToolCallEXT {
    XrStructureType          type; //!< Must be XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT
    const void* XR_MAY_ALIAS next;
    XrSession                session;
    uint64_t                 callId;   //!< Correlates xrGet…Args/xrSubmit…Result; never 0
    char                     toolName[XR_MAX_MCP_TOOL_NAME_SIZE_EXT]; //!< NUL-terminated
    uint32_t                 argsSize; //!< Bytes incl. NUL for xrGetMCPToolCallArgsEXT
} XrEventDataMCPToolCallEXT;

/*!
 * @brief Fetch a pending call's JSON arguments (two-call idiom).
 *
 * @param capacity     Size of @p buffer in bytes; 0 to query.
 * @param countOutput  Required size incl. NUL.
 * @return XR_SUCCESS, XR_ERROR_SIZE_INSUFFICIENT,
 *         XR_ERROR_VALIDATION_FAILURE (unknown/expired callId),
 *         XR_ERROR_HANDLE_INVALID.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetMCPToolCallArgsEXT)(
    XrSession                session,
    uint64_t                 callId,
    uint32_t                 capacity,
    uint32_t*                countOutput,
    char*                    buffer);

/*!
 * @brief Answer a pending tool call.
 *
 * @param success     XR_TRUE → @p resultJson becomes the tool result;
 *                    XR_FALSE → the agent receives a tool error.
 * @param resultJson  NUL-terminated JSON value (object recommended).
 *                    NULL/empty is treated as `{}`. Copied.
 * @return XR_SUCCESS (also for late results on an expired callId —
 *         the result is dropped), XR_ERROR_VALIDATION_FAILURE
 *         (callId never existed), XR_ERROR_HANDLE_INVALID.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSubmitMCPToolResultEXT)(
    XrSession                session,
    uint64_t                 callId,
    XrBool32                 success,
    const char*              resultJson);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrGetMCPToolCallArgsEXT(
    XrSession                session,
    uint64_t                 callId,
    uint32_t                 capacity,
    uint32_t*                countOutput,
    char*                    buffer);
XRAPI_ATTR XrResult XRAPI_CALL xrSubmitMCPToolResultEXT(
    XrSession                session,
    uint64_t                 callId,
    XrBool32                 success,
    const char*              resultJson);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_MCP_TOOLS_H
