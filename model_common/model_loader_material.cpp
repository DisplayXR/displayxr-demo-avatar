// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the shared material/texture helpers.
 */

#include "model_loader_material.h"
#include "stb_image.h"   // declarations only; impl linked from common/ (see model_loader_gltf.cpp)

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace {

int appendDecoded(const unsigned char* bytes, int size, ModelData& out) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);  // force RGBA
    if (!data) return -1;
    ModelTexture t;
    t.width = w;
    t.height = h;
    t.rgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    out.textures.push_back(std::move(t));
    return (int)out.textures.size() - 1;
}

}  // namespace

int material_load_texture_file(const std::string& path, ModelData& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return -1;
    const std::streamsize sz = f.tellg();
    if (sz <= 0) return -1;
    f.seekg(0);
    std::vector<unsigned char> buf((size_t)sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return appendDecoded(buf.data(), (int)sz, out);
}

int material_load_texture_memory(const unsigned char* bytes, int size, ModelData& out) {
    if (!bytes || size <= 0) return -1;
    return appendDecoded(bytes, size, out);
}

float material_shininess_to_roughness(float shininess) {
    const float s = std::max(0.0f, shininess);
    const float r = 1.0f - std::sqrt(std::min(s, 1000.0f) / 1000.0f);
    return std::clamp(r, 0.04f, 1.0f);
}
