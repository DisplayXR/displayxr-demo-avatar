// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_cocoa_window_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to provide its own NSView
 * (with CAMetalLayer backing) to the runtime on macOS. When provided,
 * the runtime will render into the application's view instead of creating
 * its own window.
 *
 * This enables:
 * - Windowed mode rendering (vs fullscreen)
 * - Application control over window input (keyboard, mouse)
 * - Multiple OpenXR applications on the same display
 * - HUD overlays and custom UI compositing
 * - Offscreen readback: viewHandle=NULL + readbackCallback → composited
 *   pixels delivered to callback instead of presented to a window
 *
 * The app provides an NSView subclass whose -makeBackingLayer returns
 * a CAMetalLayer. MoltenVK creates its VkSurfaceKHR from this layer.
 * Alternatively, set viewHandle=NULL and provide a readbackCallback to
 * receive composited pixels via GPU readback (no window required).
 */
#ifndef XR_EXT_COCOA_WINDOW_BINDING_H
#define XR_EXT_COCOA_WINDOW_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_cocoa_window_binding 1
#define XR_EXT_cocoa_window_binding_SPEC_VERSION 6
#define XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_cocoa_window_binding"

// Use a value in the vendor extension range (1000000000+)
// This should be replaced with an official Khronos-assigned value if the extension is standardized
#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999004)

// Window-space composition layer (shared with XR_EXT_win32_window_binding)
#ifndef XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)

/*!
 * @brief Composition layer positioned in fractional window coordinates.
 *
 * Renders a textured quad at a position specified as fractions of the target
 * window dimensions. Coordinates automatically scale when the window is resized.
 * The same texture is composited into both eye views with a per-eye horizontal
 * shift controlled by the disparity parameter.
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
#endif

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide
 *        an external NSView handle for session rendering on macOS.
 *
 * When this structure is provided in the next chain of XrSessionCreateInfo,
 * the runtime will render into the specified view instead of creating
 * its own window. The application is responsible for:
 * - Creating and managing the NSWindow + NSView lifecycle
 * - Running the NSApplication event loop
 * - Processing input events
 *
 * The viewHandle must point to an NSView whose backing layer is a
 * CAMetalLayer (i.e., -makeBackingLayer returns [CAMetalLayer layer]).
 *
 * Alternatively, set viewHandle to NULL and provide a readbackCallback
 * to receive composited pixels via GPU readback (offscreen mode).
 *
 * @extends XrSessionCreateInfo
 */
typedef void (*PFN_xrReadbackCallback)(
    const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

typedef struct XrCocoaWindowBindingCreateInfoEXT {
    XrStructureType          type;              //!< Must be XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS next;              //!< Pointer to next structure in chain
    void*                    viewHandle;        //!< NSView* with CAMetalLayer backing, or NULL for offscreen
    PFN_xrReadbackCallback   readbackCallback;  //!< Called with composited RGBA pixels (offscreen mode)
    void*                    readbackUserdata;   //!< Passed to readbackCallback
    void*                    sharedIOSurface;   //!< IOSurfaceRef for zero-copy Metal texture sharing, or NULL
    XrBool32                 transparentBackgroundEnabled; //!< When XR_TRUE, NSWindow + CAMetalLayer
                                                           //!< are configured with isOpaque=NO so the
                                                           //!< desktop shows through transparent
                                                           //!< (alpha < 1) regions of the composited
                                                           //!< output. Per-pixel alpha is preserved
                                                           //!< end-to-end via sim_display's alpha-native
                                                           //!< output stage; no chroma-key trick needed
                                                           //!< on macOS. (Sibling of
                                                           //!< XrWin32WindowBindingCreateInfoEXT.transparentBackgroundEnabled.)
} XrCocoaWindowBindingCreateInfoEXT;

// ---- Canvas Sub-Rect (Shared Texture Output Rect) ----

/*!
 * @brief Set the canvas sub-rect within the app's view where 3D content appears.
 *
 * macOS equivalent of the same function in XR_EXT_win32_window_binding.
 * See the Win32 spec for full documentation.
 *
 * Coordinates are relative to the NSView's bounds (not screen-space).
 * When this function is never called, the runtime assumes the full view
 * bounds are the canvas.
 *
 * @param session The session (must have been created with a window binding).
 * @param x       Left edge of the canvas in view pixels.
 * @param y       Top edge of the canvas in view pixels.
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
 * @brief Register a full-view 2D IOSurface whose pixels OUTSIDE the canvas
 *        sub-rect are blitted into the target swapchain each frame.
 *
 * macOS counterpart to the Win32 version. Signature, lifecycle, and semantics
 * are identical except: sharedTextureHandle is an IOSurfaceRef (cast to void*),
 * synchronization uses Metal fences + IOSurface use-count rather than
 * IDXGIKeyedMutex, and pixel format must be MTLPixelFormatRGBA8Unorm or
 * MTLPixelFormatRGBA8Unorm_sRGB.
 *
 * See XR_EXT_win32_window_binding.h for the full semantic description.
 *
 * @param session             The session (must have been created with a
 *                            window binding).
 * @param sharedTextureHandle IOSurfaceRef for the 2D surround texture, or NULL
 *                            to clear.
 * @param width               Texture width in physical (post-Retina) pixels.
 *                            Must equal the NSView backing-store width.
 * @param height              Texture height in physical pixels. Must equal
 *                            the NSView backing-store height.
 *
 * @return XR_SUCCESS on success.
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the extension is not enabled.
 *         XR_ERROR_VALIDATION_FAILURE if the dimensions do not match the
 *         current NSView backing size.
 *         XR_ERROR_HANDLE_INVALID if the IOSurface cannot be referenced.
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

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_COCOA_WINDOW_BINDING_H
