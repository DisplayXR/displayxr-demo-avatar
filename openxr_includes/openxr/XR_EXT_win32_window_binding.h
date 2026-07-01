// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Header for XR_EXT_win32_window_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to provide its own window handle
 * (HWND on Windows) to the runtime. When provided, the runtime will render
 * into the application's window instead of creating its own window.
 *
 * This enables:
 * - Windowed mode rendering (vs fullscreen)
 * - Application control over window input (keyboard, mouse)
 * - Multiple OpenXR applications on the same display
 * - Offscreen readback: windowHandle=NULL + readbackCallback → composited
 *   pixels delivered to callback instead of presented to a window
 * - Zero-copy shared texture: windowHandle=NULL + sharedTextureHandle →
 *   runtime composites into a shared D3D11/D3D12 texture (HANDLE)
 */
#ifndef XR_EXT_WIN32_WINDOW_BINDING_H
#define XR_EXT_WIN32_WINDOW_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_win32_window_binding 1
#define XR_EXT_win32_window_binding_SPEC_VERSION 7
#define XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_win32_window_binding"

// Use a value in the vendor extension range (1000000000+)
// This should be replaced with an official Khronos-assigned value if the extension is standardized
#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)

/*!
 * @brief Callback for offscreen readback mode (CPU fallback).
 *
 * When set in XrWin32WindowBindingCreateInfoEXT with windowHandle=NULL,
 * the runtime delivers composited RGBA pixels via this callback each frame.
 *
 * @param pixels   Pointer to RGBA pixel data (width * height * 4 bytes)
 * @param width    Image width in pixels
 * @param height   Image height in pixels
 * @param userdata Opaque pointer from readbackUserdata
 */
typedef void (*PFN_xrReadbackCallback)(
    const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide
 *        an external window handle for session rendering.
 *
 * When this structure is provided in the next chain of XrSessionCreateInfo,
 * the runtime will render into the specified window instead of creating
 * its own window. The application is responsible for:
 * - Creating and managing the window lifecycle
 * - Handling the window message pump
 * - Processing input events
 *
 * Alternatively, set windowHandle to NULL and provide either:
 * - readbackCallback for CPU-side offscreen readback (GPU→CPU round-trip), or
 * - sharedTextureHandle for zero-copy GPU texture sharing (D3D11/D3D12 HANDLE)
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType             type;                  //!< Must be XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS    next;                  //!< Pointer to next structure in chain
#ifdef _WIN32
    void*                       windowHandle;          //!< HWND of the target window (Windows only)
#else
    void*                       windowHandle;          //!< Platform-specific window handle (reserved)
#endif
    PFN_xrReadbackCallback      readbackCallback;      //!< Offscreen readback callback (CPU fallback), or NULL
    void*                       readbackUserdata;      //!< Passed to readbackCallback
    void*                       sharedTextureHandle;   //!< Shared D3D11/D3D12 texture HANDLE for zero-copy, or NULL
    //! When XR_TRUE, the runtime configures the bound HWND for transparent desktop
    //! composition: pixels written by the app with full opacity appear opaque on screen,
    //! and pixels written transparent (alpha = 0, or matching a chroma key set by the
    //! app via SetLayeredWindowAttributes) compose through to the desktop underneath.
    //! The runtime picks the appropriate DXGI / Windows mechanism per graphics API;
    //! apps should not depend on which one. Only honored when windowHandle is non-NULL
    //! and the session is standalone — ignored in workspace/shell mode.
    XrBool32                    transparentBackgroundEnabled;
    //! Optional chroma-key color used by the runtime's post-weave alpha-conversion pass
    //! when transparentBackgroundEnabled = XR_TRUE. Format: 0x00BBGGRR (Win32 COLORREF).
    //! When non-zero, the runtime samples the post-weave swapchain image and writes
    //! alpha = 0 for pixels whose RGB exactly matches this color (alpha = 1 otherwise),
    //! so the bound display processor's weaver — which strips per-pixel alpha during
    //! interlacing — does not block transparency. The application is responsible for
    //! clearing eye views to the same color in transparent regions.
    //! Set to 0 to disable the post-weave pass (relies entirely on the swapchain's
    //! per-pixel alpha; usable with alpha-respecting display processors only).
    uint32_t                    chromaKeyColor;
} XrWin32WindowBindingCreateInfoEXT;

/*!
 * @brief Composition layer positioned in fractional window coordinates.
 *
 * This layer type renders a textured quad at a position specified as fractions
 * of the target window dimensions. The coordinates automatically scale when the
 * window is resized.
 *
 * The same texture is composited into both eye views with a per-eye horizontal
 * shift controlled by the disparity parameter. The layer is rendered pre-interlace
 * (passes through the weaver like any other layer).
 *
 * This layer type is only valid when the session was created with
 * XrWin32WindowBindingCreateInfoEXT (i.e., rendering to an application-provided window).
 *
 * @extends XrFrameEndInfo (submitted as a composition layer)
 */
typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    XrCompositionLayerFlags     layerFlags; //!< e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    XrSwapchainSubImage         subImage;   //!< Source swapchain + rect
    float                       x;          //!< Left edge, fraction of window width  [0..1]
    float                       y;          //!< Top edge, fraction of window height   [0..1]
    float                       width;      //!< Fraction of window width  [0..1]
    float                       height;     //!< Fraction of window height [0..1]
    float                       disparity;  //!< Horizontal shift, fraction of window width.
                                            //!< 0 = screen depth, negative = toward viewer
} XrCompositionLayerWindowSpaceEXT;

// ---- Canvas Sub-Rect (Shared Texture Output Rect) ----

/*!
 * @brief Set the canvas sub-rect within the app's window where 3D content appears.
 *
 * For _texture apps (shared texture mode), the 3D canvas may be a sub-rect of
 * the app's window — e.g., a 3D viewport surrounded by 2D toolbars. The runtime
 * needs this rect to:
 * - Compute correct interlacing alignment (screen-space position matters for
 *   lenticular displays — see spec §2.4 "The Phase Alignment Problem")
 * - Size views and Kooima projection based on canvas dimensions, not window size
 *
 * Call this whenever the canvas position or size changes (e.g., on window resize
 * or layout change). For static layouts, call once after session creation.
 *
 * Coordinates are relative to the HWND client area (not screen-space).
 * When this function is never called, the runtime assumes the full window
 * client area is the canvas.
 *
 * @param session The session (must have been created with a window binding).
 * @param x       Left edge of the canvas in client-area pixels.
 * @param y       Top edge of the canvas in client-area pixels.
 * @param width   Canvas width in pixels.
 * @param height  Canvas height in pixels.
 *
 * @return XR_SUCCESS on success.
 */
#ifndef PFN_xrSetSharedTextureOutputRectEXT_DEFINED
#define PFN_xrSetSharedTextureOutputRectEXT_DEFINED
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureOutputRectEXT)(
    XrSession session, int32_t x, int32_t y, uint32_t width, uint32_t height);
#endif

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetSharedTextureOutputRectEXT(
    XrSession                           session,
    int32_t                             x,
    int32_t                             y,
    uint32_t                            width,
    uint32_t                            height);
#endif

// ---- 2D Surround Texture (Spec v6) ----

/*!
 * @brief Register a full-window 2D shared texture whose pixels OUTSIDE the
 *        canvas sub-rect are blitted into the target swapchain each frame.
 *
 * Pairs with xrSetSharedTextureOutputRectEXT for _texture apps that want to
 * fill the non-3D part of their window with full-resolution 2D content (UI,
 * chrome, toolbars, status bars). The runtime samples this texture each frame
 * and copies the pixels OUTSIDE the active canvas rect into the corresponding
 * region of the target swapchain. Pixels INSIDE the canvas rect are ignored —
 * that region is owned by the display processor's weaved output.
 *
 * The texture must be a D3D11 or D3D12 shared texture (NT HANDLE), sized to
 * match the target swapchain dimensions (HWND client area in physical pixels).
 * The runtime opens the handle once and re-uses it across frames; the app
 * writes into the texture asynchronously and the runtime samples at frame
 * submit time. Synchronization follows the same IDXGIKeyedMutex pattern as
 * the multiview shared texture registered via XrWin32WindowBindingCreateInfoEXT
 * (key 0 = "app done writing, runtime may read").
 *
 * Lifecycle:
 * - Call with a non-NULL handle to register or replace the surround texture.
 * - Call with NULL handle to clear — runtime falls back to undefined non-canvas
 *   pixels (the spec v5 behavior).
 * - If the window is resized, the app must allocate a new shared texture at
 *   the new size and call this function again.
 *
 * Texture format: DXGI_FORMAT_R8G8B8A8_UNORM or DXGI_FORMAT_R8G8B8A8_UNORM_SRGB.
 * The runtime selects the matching SRV at sample time so linearization is
 * correct regardless of which format the app chose.
 *
 * @param session             The session (must have been created with a
 *                            window binding).
 * @param sharedTextureHandle Shared D3D11/D3D12 texture HANDLE for the 2D
 *                            surround content, or NULL to clear.
 * @param width               Texture width in pixels. Must equal the HWND
 *                            client-area width.
 * @param height              Texture height in pixels. Must equal the HWND
 *                            client-area height.
 *
 * @return XR_SUCCESS on success.
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the extension is not enabled.
 *         XR_ERROR_VALIDATION_FAILURE if the dimensions do not match the
 *         current HWND client area.
 *         XR_ERROR_HANDLE_INVALID if the shared handle cannot be opened.
 */
#ifndef PFN_xrSetSharedTextureSurround2DEXT_DEFINED
#define PFN_xrSetSharedTextureSurround2DEXT_DEFINED
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureSurround2DEXT)(
    XrSession session,
    void*     sharedTextureHandle,
    uint32_t  width,
    uint32_t  height);
#endif

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetSharedTextureSurround2DEXT(
    XrSession                           session,
    void*                               sharedTextureHandle,
    uint32_t                            width,
    uint32_t                            height);
#endif

// ---- 2D Surround Texture with Fence Sync (Spec v7) ----

/*!
 * @brief D3D12 variant of xrSetSharedTextureSurround2DEXT that uses an
 *        ID3D12Fence for producer→consumer synchronization instead of
 *        IDXGIKeyedMutex.
 *
 * Rationale: D3D12-native shared resources (those created via
 * D3D12_HEAP_FLAG_SHARED + ID3D12Device::CreateSharedHandle) do not reliably
 * expose IDXGIKeyedMutex when re-opened via OpenSharedHandle — QueryInterface
 * returns E_NOINTERFACE on common Windows/driver combinations. ID3D12Fence is
 * the canonical D3D12 cross-process sync primitive and is shareable via
 * CreateSharedHandle.
 *
 * Semantics:
 *   1. App allocates the surround texture as a D3D12 SHARED resource (no
 *      KeyedMutex needed) and a shared ID3D12Fence. Exports NT handles for
 *      both via ID3D12Device::CreateSharedHandle.
 *   2. Each frame, after recording surround content into the texture and
 *      calling ExecuteCommandLists, the app calls
 *      ID3D12CommandQueue::Signal(fence, value).
 *   3. App calls this function with the same handles + the value it just
 *      signaled.
 *   4. The runtime caches both handles on first call (opened via
 *      ID3D12Device::OpenSharedHandle for the texture and OpenSharedFence
 *      for the fence). On subsequent calls with the same handle values, it
 *      reuses the cached opens and just updates the await value.
 *   5. At submit time the runtime issues ID3D12CommandQueue::Wait(fence,
 *      awaitFenceValue) on its own queue before the strip blit reads from
 *      the surround texture. Read-only access, so no signal-back is needed.
 *   6. Per-frame fenceValue MUST monotonically increase. The first call may
 *      use any value; subsequent calls must use strictly greater values, or
 *      the runtime may block forever on the queue wait.
 *   7. Call with NULL sharedTextureHandle to clear (the fence handle is
 *      ignored in the clear case).
 *
 * The D3D11 keyed-mutex path (xrSetSharedTextureSurround2DEXT) and the D3D12
 * fence path are mutually exclusive per session. The runtime tracks which one
 * the app used and rejects the other for the same session.
 *
 * @param session             The session.
 * @param sharedTextureHandle Shared D3D12 texture NT HANDLE, or NULL to clear.
 * @param width               Texture width in pixels.
 * @param height              Texture height in pixels.
 * @param sharedFenceHandle   Shared ID3D12Fence NT HANDLE. May be NULL only
 *                            when sharedTextureHandle is NULL (clear case).
 * @param awaitFenceValue     Fence value the runtime should wait on before
 *                            reading the surround texture this frame. Must
 *                            be strictly greater than the previous frame's
 *                            value once steady-state begins.
 *
 * @return XR_SUCCESS on success.
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the runtime predates spec v7 or
 *           the active compositor does not support fence-based surround
 *           (currently D3D12 only).
 *         XR_ERROR_HANDLE_INVALID if either handle cannot be opened.
 *         XR_ERROR_VALIDATION_FAILURE on dimension mismatch or non-monotonic
 *           awaitFenceValue.
 */
#ifndef PFN_xrSetSharedTextureSurround2DFenceEXT_DEFINED
#define PFN_xrSetSharedTextureSurround2DFenceEXT_DEFINED
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureSurround2DFenceEXT)(
    XrSession session,
    void*     sharedTextureHandle,
    uint32_t  width,
    uint32_t  height,
    void*     sharedFenceHandle,
    uint64_t  awaitFenceValue);
#endif

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetSharedTextureSurround2DFenceEXT(
    XrSession                           session,
    void*                               sharedTextureHandle,
    uint32_t                            width,
    uint32_t                            height,
    void*                               sharedFenceHandle,
    uint64_t                            awaitFenceValue);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_WIN32_WINDOW_BINDING_H
