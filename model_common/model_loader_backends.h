// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Internal per-format loader backends behind model_loader_load().
 *
 * The public entry point model_loader_load() (model_loader.cpp) inspects the
 * file extension and dispatches to one of these backends. Each backend fills a
 * ModelData from one file and obeys the same contract as the public API:
 * returns false on parse failure or when no drawable geometry was found. All
 * backends funnel into the identical ModelData the renderer already consumes —
 * adding a format is front-end work only; the renderer is untouched.
 *
 * Not part of the public surface; platform code includes model_loader.h.
 */

#pragma once

#include "model_loader.h"

// .glb / .gltf — tinygltf. PBR-native. (model_loader_gltf.cpp)
bool model_load_gltf(const char* path, ModelData& out);

// .stl — hand-rolled binary+ASCII parser, no dependency. Geometry only; a
// single neutral default material (STL carries none). (model_loader_stl.cpp)
bool model_load_stl(const char* path, ModelData& out);

// .obj — tinyobjloader. Legacy (Phong/.mtl) materials shimmed to
// metallic-roughness via model_loader_material.h. (model_loader_obj.cpp)
bool model_load_obj(const char* path, ModelData& out);

// .fbx — ufbx. PBR maps preferred, legacy Phong shimmed; static geometry only
// (skinning/animation deferred). (model_loader_fbx.cpp)
bool model_load_fbx(const char* path, ModelData& out);

// .usdz / .usd / .usda / .usdc — tinyusdz. PBR-native (UsdPreviewSurface).
// (model_loader_usd.cpp)
bool model_load_usd(const char* path, ModelData& out);
