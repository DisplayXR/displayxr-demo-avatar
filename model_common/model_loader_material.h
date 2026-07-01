// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Shared material/texture helpers for the non-glTF mesh backends.
 *
 * OBJ (tinyobjloader) and FBX (ufbx) both arrive with legacy or mixed material
 * models and reference textures by external path or embedded blob. These
 * helpers do the common work: decode an image to RGBA8 and append it to
 * ModelData::textures (returning its index), and approximate a Phong specular
 * exponent as a metallic-roughness roughness. Decoding goes through
 * stbi_load_from_memory (the impl already linked from common/), matching the
 * glTF backend — textures are uploaded UNORM and sRGB-decoded in the shader.
 */

#pragma once

#include "model_loader.h"
#include <string>

// Decode an image FILE → RGBA8, append to out.textures, return its index (or -1
// on open/decode failure). Reads the bytes itself then stbi_load_from_memory,
// so it does not depend on stb's stdio path being compiled in.
int material_load_texture_file(const std::string& path, ModelData& out);

// Decode an in-memory compressed image (PNG/JPEG bytes) → RGBA8, append to
// out.textures, return its index (or -1). For embedded textures (FBX).
int material_load_texture_memory(const unsigned char* bytes, int size, ModelData& out);

// Phong specular exponent → perceptual roughness in [0.04, 1]. High exponent =
// smooth surface = low roughness.
float material_shininess_to_roughness(float shininess);
