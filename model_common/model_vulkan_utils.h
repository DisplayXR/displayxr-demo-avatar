// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Vulkan buffer/image creation helpers for glTF model renderer
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

// Find a memory type index matching the required properties.
uint32_t modelFindMemoryType(VkPhysicalDevice physDevice,
                          uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);

struct ModelBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

// Create a GPU buffer with the specified usage and memory properties.
ModelBuffer modelCreateBuffer(VkDevice device,
                        VkPhysicalDevice physDevice,
                        VkDeviceSize size,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags memProps);

// Destroy a buffer and free its memory.
void modelDestroyBuffer(VkDevice device, ModelBuffer& buf);

struct ModelImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Create a 2D image with a view.
ModelImage modelCreateImage2D(VkDevice device,
                        VkPhysicalDevice physDevice,
                        uint32_t width,
                        uint32_t height,
                        VkFormat format,
                        VkImageUsageFlags usage);

// Destroy an image, its view, and free memory.
void modelDestroyImage(VkDevice device, ModelImage& img);

// Upload CPU data to a device-local buffer via a staging buffer.
bool modelUploadBuffer(VkDevice device,
                    VkPhysicalDevice physDevice,
                    VkQueue queue,
                    VkCommandPool cmdPool,
                    ModelBuffer& dst,
                    const void* data,
                    VkDeviceSize size);
