// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Model-format dispatcher + path helpers.
 *
 * model_loader_load() inspects the file extension and routes to a per-format
 * backend (model_loader_backends.h): glTF → tinygltf, OBJ/STL/FBX → Assimp,
 * USD(Z) → tinyusdz. Every backend fills the same ModelData the renderer
 * consumes, so the renderer never learns which format it came from. Only the
 * glTF backend is implemented today; the others are reachable stubs.
 */

#include "model_loader.h"
#include "model_loader_backends.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

enum class ModelFormat { Unknown, Gltf, Obj, Stl, Fbx, Usd };

// Lowercased file extension (with leading dot), e.g. ".glb".
std::string lowerExt(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

ModelFormat formatFromPath(const std::string& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".glb"  || ext == ".gltf")                 return ModelFormat::Gltf;
    if (ext == ".obj")                                    return ModelFormat::Obj;
    if (ext == ".stl")                                    return ModelFormat::Stl;
    if (ext == ".fbx")                                    return ModelFormat::Fbx;
    if (ext == ".usdz" || ext == ".usd" ||
        ext == ".usda" || ext == ".usdc")                 return ModelFormat::Usd;
    return ModelFormat::Unknown;
}

}  // namespace

bool model_loader_load(const char* path, ModelData& out) {
    if (!path) return false;
    switch (formatFromPath(path)) {
        case ModelFormat::Gltf:
            return model_load_gltf(path, out);
        case ModelFormat::Stl:
            return model_load_stl(path, out);
        case ModelFormat::Obj:
            return model_load_obj(path, out);
        case ModelFormat::Fbx:
            return model_load_fbx(path, out);
        case ModelFormat::Usd:
            return model_load_usd(path, out);
        default:
            std::fprintf(stderr, "[model_loader] '%s': unsupported format\n", path);
            return false;
    }
}

// ── Path helpers ──────────────────────────────────────────────────────────
// model_validate_file gates the file-open dialogs and drag-drop. It lists only
// extensions whose backend actually loads today — keep it in lockstep with the
// implemented backends (widen as model_load_assimp / model_load_usd land) so
// the UI never offers a format that fails on load.
bool model_validate_file(const std::string& path) {
    if (path.empty()) return false;
    const std::string ext = lowerExt(path);
    static constexpr std::array<const char*, 9> kSupported = {
        ".glb", ".gltf", ".stl", ".obj", ".fbx", ".usdz", ".usd", ".usda", ".usdc" };
    bool known = false;
    for (const char* e : kSupported) if (ext == e) { known = true; break; }
    if (!known) return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

std::string model_basename(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

std::string model_filesize_str(const std::string& path) {
    try {
        auto size = std::filesystem::file_size(path);
        char buf[32];
        if (size >= 1024ull * 1024 * 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024 * 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f MB", (double)size / (1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f KB", (double)size / 1024.0);
            return buf;
        }
        return std::to_string(size) + " B";
    } catch (...) {
        return "unknown";
    }
}
