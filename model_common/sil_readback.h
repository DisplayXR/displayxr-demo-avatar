// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Frame-ring-pipelined silhouette readback, shared by every leg.
 *
 * The click-through / gesture coverage renders the avatar into small scratch
 * images and reads their alpha back on the CPU. It used to do that with its
 * own command buffers: allocate, submit, vkQueueWaitIdle (Android, macOS) or a
 * private fence waited one frame later (Windows, Linux) — a second and third
 * vkQueueSubmit per frame, and on Android a mid-frame queue drain that parked
 * the Adreno governor (displayxr-runtime INV-4.10, #1917).
 *
 * This helper records the copies into ModelRenderer's ONE frame command buffer
 * instead. Each ring slot owns a band of the host-visible readback buffer; the
 * bytes a frame copied are read when the SAME slot opens again, i.e. after the
 * ring has waited that slot's fence — framesInFlight frames later, never by
 * waiting. Per frame:
 *
 *   take()      after the frame opened: the band this slot wrote N frames ago
 *   ensure()    (re)size the scratch images + bands (old ones deferred-destroyed)
 *   prepFresh() new images → cleared COLOR_ATTACHMENT_OPTIMAL (avatar#99)
 *   renderEye() into image(i), then record(i, plane) — in the frame's buffer
 *   commit()    remember what the band holds (dims + caller metadata)
 *
 * Render thread only.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include "model_renderer.h"
#include "model_vulkan_utils.h"

struct SilReadback {
    static constexpr uint32_t kMaxImages = 2;   //!< scratch images (= max planes)
    static constexpr uint32_t kMaxSlots = dxr::detail::kMaxFramesInFlight;

    //! What one ring slot's band holds. `user` is caller metadata recorded
    //! with the copy (zone rect, raster row offset, window size, ...), because
    //! the caller's state has moved on by the time the slot comes around.
    struct Meta {
        uint32_t w = 0, h = 0, planes = 0;
        int32_t user[8] = {};
    };

    ModelImage img[kMaxImages] = {};
    uint32_t nImages = 0;
    ModelBuffer buf = {};
    void* mapped = nullptr;
    VkDeviceSize planeBytes = 0;     //!< w*h*4
    VkDeviceSize slotStride = 0;     //!< planeBytes * kMaxImages
    bool fresh = false;              //!< images UNDEFINED until prepFresh()
    bool hostCached = false;         //!< readback memory is HOST_CACHED
    bool pending[kMaxSlots] = {};
    Meta meta[kMaxSlots];

    VkImage image(uint32_t i) const { return i < nImages ? img[i].image : VK_NULL_HANDLE; }

    //! Planes of the band ring slot frameSlot() wrote framesInFlight frames
    //! ago, if any. Call after the frame opened (frameCommandBuffer()/renderEye)
    //! — that is when the renderer waited the slot's fence. Valid until the
    //! next record() into this slot.
    bool take(ModelRenderer& r, const uint8_t** outPx, Meta* outMeta) {
        if (r.frameCommandBuffer() == VK_NULL_HANDLE || mapped == nullptr) return false;
        const uint32_t slot = r.frameSlot();
        if (slot >= kMaxSlots || !pending[slot]) return false;
        pending[slot] = false;
        *outPx = static_cast<const uint8_t*>(mapped) + slotStride * slot;
        *outMeta = meta[slot];
        return true;
    }

    //! (Re)create `images` w x h RGBA8 scratch images and framesInFlight bands.
    //! The old objects may still be read by frames in flight, so they go to the
    //! renderer's deferred destroy; every pending band is dropped. HOST_CACHED
    //! is preferred when asked: the band is READ every frame, and plain
    //! HOST_VISIBLE|COHERENT is typically write-combined on a discrete GPU —
    //! measured ~127 ms vs ~0.86 ms per frame on an RTX 3080 (#837).
    bool ensure(ModelRenderer& r, VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h,
                uint32_t images, bool preferHostCached) {
        if (images == 0 || images > kMaxImages) return false;
        bool ok = nImages == images && buf.buffer != VK_NULL_HANDLE && mapped != nullptr;
        for (uint32_t i = 0; ok && i < images; ++i)
            ok = img[i].image != VK_NULL_HANDLE && img[i].width == w && img[i].height == h;
        if (ok) return true;

        retire(r, dev);
        // avatar#99: all three usages are load-bearing. TRANSFER_DST for
        // renderEye's blit and the clear, TRANSFER_SRC for the copy-out, and
        // COLOR_ATTACHMENT because renderEye's barriers name
        // COLOR_ATTACHMENT_OPTIMAL and modelCreateImage2D always creates a view.
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        for (uint32_t i = 0; i < images; ++i) {
            img[i] = modelCreateImage2D(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, usage);
            if (img[i].image == VK_NULL_HANDLE) { retire(r, dev); return false; }
        }
        nImages = images;
        fresh = true;
        planeBytes = (VkDeviceSize)w * h * 4;
        slotStride = planeBytes * kMaxImages;
        const VkDeviceSize bytes = slotStride * r.framesInFlight();
        const VkMemoryPropertyFlags coherent =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        hostCached = false;
        if (preferHostCached) {
            buf = modelCreateBuffer(dev, phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            hostCached = buf.buffer != VK_NULL_HANDLE;
        }
        if (buf.buffer == VK_NULL_HANDLE)
            buf = modelCreateBuffer(dev, phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, coherent);
        if (buf.buffer == VK_NULL_HANDLE) { retire(r, dev); return false; }
        if (vkMapMemory(dev, buf.memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            mapped = nullptr;
            retire(r, dev);
            return false;
        }
        return true;
    }

    //! Bring freshly created images UNDEFINED → cleared COLOR_ATTACHMENT_OPTIMAL,
    //! inside the frame. renderEye picks its entry layout from
    //! firstViewInImage = (viewportX == 0 && viewportY == 0); a silhouette drawn
    //! into a bottom band always claims COLOR_ATTACHMENT_OPTIMAL (avatar#99).
    void prepFresh(ModelRenderer& r) {
        if (!fresh) return;
        VkCommandBuffer cmd = r.frameCommandBuffer();
        if (cmd == VK_NULL_HANDLE) return;
        for (uint32_t i = 0; i < nImages; ++i)
            clearToColorAttachment(cmd, img[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        fresh = false;
    }

    //! Record image(i) → plane `plane` of this slot's band (after renderEye left
    //! it in COLOR_ATTACHMENT_OPTIMAL). `restoreCleared` hands the image back
    //! cleared in COLOR_ATTACHMENT_OPTIMAL for the next bottom-band renderEye.
    //! Returns the slot recorded into (UINT32_MAX on failure); a caller that
    //! records several planes must see the SAME slot for all of them — a
    //! target resize inside renderEye can roll the frame onto the next slot.
    uint32_t record(ModelRenderer& r, uint32_t i, uint32_t plane, bool restoreCleared) {
        if (i >= nImages || plane >= kMaxImages || buf.buffer == VK_NULL_HANDLE) return UINT32_MAX;
        const uint32_t slot = r.recordColorReadback(img[i].image, img[i].width, img[i].height,
                                                    buf.buffer, slotStride, planeBytes * plane);
        if (slot != UINT32_MAX && restoreCleared) {
            clearToColorAttachment(r.frameCommandBuffer(), img[i].image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        return slot;
    }

    //! Mark `slot`'s band as holding `m` (read back when the slot reopens).
    void commit(uint32_t slot, const Meta& m) {
        if (slot >= kMaxSlots) return;
        pending[slot] = true;
        meta[slot] = m;
    }

    //! Teardown (device idle): free everything now.
    void destroy(VkDevice dev) {
        if (mapped != nullptr) { vkUnmapMemory(dev, buf.memory); mapped = nullptr; }
        if (buf.buffer != VK_NULL_HANDLE) modelDestroyBuffer(dev, buf);
        for (uint32_t i = 0; i < kMaxImages; ++i)
            if (img[i].image != VK_NULL_HANDLE) modelDestroyImage(dev, img[i]);
        buf = {};
        for (auto& m : img) m = {};
        nImages = 0;
        for (auto& p : pending) p = false;
    }

private:
    void retire(ModelRenderer& r, VkDevice dev) {
        ModelImage oldImg[kMaxImages];
        for (uint32_t i = 0; i < kMaxImages; ++i) oldImg[i] = img[i];
        ModelBuffer oldBuf = buf;
        const bool wasMapped = mapped != nullptr;
        r.deferDestroy([=]() mutable {
            if (oldBuf.buffer != VK_NULL_HANDLE) {
                if (wasMapped) vkUnmapMemory(dev, oldBuf.memory);
                modelDestroyBuffer(dev, oldBuf);
            }
            for (auto& m : oldImg)
                if (m.image != VK_NULL_HANDLE) modelDestroyImage(dev, m);
        });
        for (auto& m : img) m = {};
        buf = {};
        mapped = nullptr;
        nImages = 0;
        for (auto& p : pending) p = false;
    }

    static void clearToColorAttachment(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess) {
        if (cmd == VK_NULL_HANDLE || image == VK_NULL_HANDLE) return;
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = oldLayout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const VkClearColorValue zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    }
};
