// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_macos_gl_binding extension
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
#ifndef XR_EXT_MACOS_GL_BINDING_H
#define XR_EXT_MACOS_GL_BINDING_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_macos_gl_binding 1
#define XR_EXT_macos_gl_binding_SPEC_VERSION 1
#define XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME "XR_EXT_macos_gl_binding"

// Structure type in the vendor extension range
#define XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_EXT ((XrStructureType)1000999010)

/*!
 * @brief OpenGL graphics binding for macOS (CGL context).
 *
 * Chain this structure in XrSessionCreateInfo::next to use OpenGL
 * as the graphics API. The runtime will create IOSurface-backed
 * textures that the GL client imports via CGLTexImageIOSurface2D.
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrGraphicsBindingOpenGLMacOSEXT {
    XrStructureType          type;             //!< Must be XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_EXT
    const void* XR_MAY_ALIAS next;             //!< Pointer to next structure in chain
    void*                    cglContext;        //!< CGLContextObj — the app's CGL rendering context
    void*                    cglPixelFormat;    //!< CGLPixelFormatObj — pixel format (may be NULL)
} XrGraphicsBindingOpenGLMacOSEXT;

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_MACOS_GL_BINDING_H
