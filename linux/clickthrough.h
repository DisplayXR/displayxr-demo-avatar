// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 click-through for the transparent avatar overlay (runtime#757).
 *
 * The Linux analogue of the Windows SetWindowRgn / macOS setIgnoresMouseEvents
 * silhouette click-through: each frame render a downscaled mono view of the
 * avatar into a scratch image, read its alpha, and set the window's XShape
 * INPUT region to just the avatar pixels — so clicks land on the avatar and pass
 * through the transparent regions to the desktop underneath.
 *
 * No-op when the window / Display is null (hosted-NULL fallback). Requires the
 * XShape extension (libXext).
 */

#pragma once

#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <cstdint>

class ModelRenderer;

//! Render the avatar silhouette and set the window's XShape input region from it.
//! Call once per frame (after the eye render) with view 0's matrices.
void ClickthroughUpdate(VkDevice dev,
                        VkPhysicalDevice phys,
                        VkQueue queue,
                        uint32_t queueFamily,
                        ModelRenderer& renderer,
                        Display* dpy,
                        Window win,
                        uint32_t winW,
                        uint32_t winH,
                        const float viewMat[16],
                        const float projMat[16]);

//! Free the scratch image / readback buffer / command pool. Device must be idle.
void ClickthroughDestroy(VkDevice dev);
