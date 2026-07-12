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
 * @brief  Header for XR_DXR_macos_gl_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to use OpenGL graphics
 * on macOS by providing its CGL context to the runtime. The runtime
 * routes GL rendering through the Metal native compositor using
 * IOSurface-backed textures for cross-API sharing.
 *
 * Pipeline: App (OpenGL) -> comp_gl_client -> Metal compositor -> CAMetalLayer
 */
#ifndef XR_DXR_MACOS_GL_BINDING_H
#define XR_DXR_MACOS_GL_BINDING_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_macos_gl_binding 1
// SPEC_VERSION 2: XrStructureType value relocated 1004999010 -> 1004999180
// (the old value collided with XR_DXR_display_info's
// XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR). No struct/field changes;
// consumers only need a header re-sync + rebuild.
#define XR_DXR_macos_gl_binding_SPEC_VERSION 1
#define XR_DXR_MACOS_GL_BINDING_EXTENSION_NAME "XR_DXR_macos_gl_binding"

// Structure type in the extension type-value range. Allocation registry:
// README.md in this directory.
#define XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_DXR ((XrStructureType)1004999180)

/*!
 * @brief OpenGL graphics binding for macOS (CGL context).
 *
 * Chain this structure in XrSessionCreateInfo::next to use OpenGL
 * as the graphics API. The runtime will create IOSurface-backed
 * textures that the GL client imports via CGLTexImageIOSurface2D.
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrGraphicsBindingOpenGLMacOSDXR {
    XrStructureType          type;             //!< Must be XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_DXR
    const void* XR_MAY_ALIAS next;             //!< Pointer to next structure in chain
    void*                    cglContext;        //!< CGLContextObj — the app's CGL rendering context
    void*                    cglPixelFormat;    //!< CGLPixelFormatObj — pixel format (may be NULL)
} XrGraphicsBindingOpenGLMacOSDXR;

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_MACOS_GL_BINDING_H
