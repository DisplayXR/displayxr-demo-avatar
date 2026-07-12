// Copyright 2025, The DisplayXR Project
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
 * @brief  Header for XR_DXR_cocoa_window_binding extension
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
#ifndef XR_DXR_COCOA_WINDOW_BINDING_H
#define XR_DXR_COCOA_WINDOW_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_cocoa_window_binding 1
#define XR_DXR_cocoa_window_binding_SPEC_VERSION 1
#define XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME "XR_DXR_cocoa_window_binding"

// Use a value in the vendor extension range (1000000000+)
// This should be replaced with an official Khronos-assigned value if the extension is standardized
#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999004)

// Window-space composition layer (shared with XR_DXR_win32_window_binding)
#ifndef XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR ((XrStructureType)1004999002)

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
typedef struct XrCompositionLayerWindowSpaceDXR {
    XrStructureType             type;       //!< Must be XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    XrCompositionLayerFlags     layerFlags; //!< e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    XrSwapchainSubImage         subImage;   //!< Source swapchain + rect
    float                       x;          //!< Left edge, fraction of window width  [0..1]
    float                       y;          //!< Top edge, fraction of window height   [0..1]
    float                       width;      //!< Fraction of window width  [0..1]
    float                       height;     //!< Fraction of window height [0..1]
    float                       disparity;  //!< Horizontal shift, fraction of window width.
                                            //!< 0 = screen depth, negative = toward viewer
} XrCompositionLayerWindowSpaceDXR;
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

typedef struct XrCocoaWindowBindingCreateInfoDXR {
    XrStructureType          type;              //!< Must be XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR
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
                                                           //!< XrWin32WindowBindingCreateInfoDXR.transparentBackgroundEnabled.)
} XrCocoaWindowBindingCreateInfoDXR;

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_COCOA_WINDOW_BINDING_H
