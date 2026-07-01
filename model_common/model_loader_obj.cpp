// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OBJ loader backend (tinyobjloader) → ModelData.
 *
 * Routed to from model_loader_load() for .obj. tinyobjloader parses the .obj
 * (+ .mtl resolved relative to it) into shared vertex/normal/texcoord pools
 * indexed per face-corner. We expand to non-indexed triangles bucketed by
 * material → one ModelPrimitive per material, identity world matrix (OBJ has no
 * scene graph). Missing normals are filled with a computed face normal; the
 * bottom-up OBJ texcoord V is flipped to the glTF/Vulkan top-down convention.
 *
 * Legacy .mtl (Phong) materials are shimmed to metallic-roughness via
 * model_loader_material.h: diffuse→baseColor, shininess→roughness, map_Kd→
 * baseColor texture, bump/normal→normal map, Pm/Pr (PBR extension) honoured when
 * present. This is the single TU that defines the tinyobjloader implementation.
 */

#define TINYOBJLOADER_IMPLEMENTATION
#include "third_party/tiny_obj_loader.h"

#include "model_loader_backends.h"
#include "model_loader_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace {

std::string resolveTex(const std::filesystem::path& dir, const std::string& name) {
    if (name.empty()) return {};
    std::filesystem::path p(name);
    return p.is_absolute() ? p.string() : (dir / p).string();
}

void accumulateBBox(ModelData& out, const float* p) {
    if (!out.hasBBox) {
        out.bboxMin[0] = out.bboxMax[0] = p[0];
        out.bboxMin[1] = out.bboxMax[1] = p[1];
        out.bboxMin[2] = out.bboxMax[2] = p[2];
        out.hasBBox = true;
    } else {
        out.bboxMin[0] = std::min(out.bboxMin[0], p[0]); out.bboxMax[0] = std::max(out.bboxMax[0], p[0]);
        out.bboxMin[1] = std::min(out.bboxMin[1], p[1]); out.bboxMax[1] = std::max(out.bboxMax[1], p[1]);
        out.bboxMin[2] = std::min(out.bboxMin[2], p[2]); out.bboxMax[2] = std::max(out.bboxMax[2], p[2]);
    }
}

}  // namespace

bool model_load_obj(const char* path, ModelData& out) {
    if (!path) return false;
    const std::filesystem::path modelDir = std::filesystem::path(path).parent_path();

    tinyobj::ObjReaderConfig cfg;
    cfg.triangulate = true;
    cfg.mtl_search_path = modelDir.string();
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg)) {
        std::fprintf(stderr, "[model_loader/obj] '%s': %s\n", path,
                     reader.Error().empty() ? "parse failed" : reader.Error().c_str());
        return false;
    }
    if (!reader.Warning().empty())
        std::fprintf(stderr, "[model_loader/obj] warn: %s\n", reader.Warning().c_str());

    const tinyobj::attrib_t&               attrib = reader.GetAttrib();
    const std::vector<tinyobj::shape_t>&   shapes = reader.GetShapes();
    const std::vector<tinyobj::material_t>& mats  = reader.GetMaterials();

    // ── Materials (+ textures). Append a neutral default for id -1 faces. ──
    auto texFile = [&](const std::string& n) -> int {
        return n.empty() ? -1 : material_load_texture_file(resolveTex(modelDir, n), out);
    };
    out.materials.reserve(mats.size() + 1);
    for (const tinyobj::material_t& m : mats) {
        ModelMaterial mm{};
        mm.baseColorFactor[0] = m.diffuse[0];
        mm.baseColorFactor[1] = m.diffuse[1];
        mm.baseColorFactor[2] = m.diffuse[2];
        mm.baseColorFactor[3] = (m.dissolve > 0.0f && m.dissolve <= 1.0f) ? m.dissolve : 1.0f;
        mm.metallic  = m.metallic;  // PBR extension; 0 by default in tinyobj
        mm.roughness = (m.roughness > 0.0f) ? m.roughness
                                            : material_shininess_to_roughness(m.shininess);
        mm.emissive[0] = m.emission[0];
        mm.emissive[1] = m.emission[1];
        mm.emissive[2] = m.emission[2];
        mm.baseColorTex = texFile(m.diffuse_texname);
        mm.normalTex    = !m.normal_texname.empty() ? texFile(m.normal_texname)
                                                    : texFile(m.bump_texname);  // bump≈normal
        mm.metallicRoughnessTex = texFile(!m.metallic_texname.empty() ? m.metallic_texname
                                                                      : m.roughness_texname);
        mm.emissiveTex = texFile(m.emissive_texname);
        out.materials.push_back(mm);
    }
    const int defaultMat = (int)out.materials.size();
    {
        ModelMaterial mm{};  // neutral grey dielectric for unmaterialed faces
        mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
        mm.baseColorFactor[3] = 1.0f;
        mm.metallic = 0.0f;
        mm.roughness = 0.8f;
        out.materials.push_back(mm);
    }

    // ── Expand triangle corners, bucketed by material id ──
    std::unordered_map<int, std::vector<ModelVertex>> buckets;
    for (const tinyobj::shape_t& shape : shapes) {
        const tinyobj::mesh_t& mesh = shape.mesh;
        size_t off = 0;
        for (size_t f = 0; f < mesh.num_face_vertices.size(); ++f) {
            const int fv = mesh.num_face_vertices[f];
            if (fv != 3) { off += fv; continue; }  // triangulated above; guard
            int matId = (f < mesh.material_ids.size() && mesh.material_ids[f] >= 0 &&
                         mesh.material_ids[f] < (int)mats.size())
                            ? mesh.material_ids[f] : defaultMat;

            const tinyobj::index_t ix[3] = { mesh.indices[off + 0],
                                             mesh.indices[off + 1],
                                             mesh.indices[off + 2] };
            const float* P[3];
            for (int k = 0; k < 3; ++k) P[k] = &attrib.vertices[3 * ix[k].vertex_index];

            // Face normal (used for any corner lacking a stored normal).
            float fn[3];
            {
                const float u[3] = { P[1][0]-P[0][0], P[1][1]-P[0][1], P[1][2]-P[0][2] };
                const float v[3] = { P[2][0]-P[0][0], P[2][1]-P[0][1], P[2][2]-P[0][2] };
                fn[0] = u[1]*v[2] - u[2]*v[1];
                fn[1] = u[2]*v[0] - u[0]*v[2];
                fn[2] = u[0]*v[1] - u[1]*v[0];
                const float l = std::sqrt(fn[0]*fn[0] + fn[1]*fn[1] + fn[2]*fn[2]);
                if (l > 1e-12f) { fn[0]/=l; fn[1]/=l; fn[2]/=l; }
                else            { fn[0]=0; fn[1]=1; fn[2]=0; }
            }

            std::vector<ModelVertex>& bucket = buckets[matId];
            for (int k = 0; k < 3; ++k) {
                ModelVertex mv{};
                mv.pos[0] = P[k][0]; mv.pos[1] = P[k][1]; mv.pos[2] = P[k][2];
                if (ix[k].normal_index >= 0) {
                    const float* n = &attrib.normals[3 * ix[k].normal_index];
                    mv.normal[0] = n[0]; mv.normal[1] = n[1]; mv.normal[2] = n[2];
                } else {
                    mv.normal[0] = fn[0]; mv.normal[1] = fn[1]; mv.normal[2] = fn[2];
                }
                if (ix[k].texcoord_index >= 0) {
                    const float* t = &attrib.texcoords[2 * ix[k].texcoord_index];
                    mv.uv[0] = t[0];
                    mv.uv[1] = 1.0f - t[1];  // OBJ V is bottom-up → flip to top-down
                }
                bucket.push_back(mv);
            }
            off += fv;
        }
    }

    // ── Assemble one primitive per material bucket ──
    for (auto& [matId, verts] : buckets) {
        if (verts.empty()) continue;
        ModelPrimitive mp{};
        mp.firstVertex = (uint32_t)out.vertices.size();
        mp.firstIndex  = (uint32_t)out.indices.size();
        mp.material = matId;
        mp.node = -1; mp.skin = -1; mp.morph = -1;
        std::memset(mp.modelMatrix, 0, sizeof(mp.modelMatrix));
        mp.modelMatrix[0] = mp.modelMatrix[5] = mp.modelMatrix[10] = mp.modelMatrix[15] = 1.0f;
        for (const ModelVertex& v : verts) {
            const uint32_t gi = (uint32_t)out.vertices.size();
            out.vertices.push_back(v);
            out.indices.push_back(gi);
            accumulateBBox(out, v.pos);
        }
        mp.vertexCount = (uint32_t)verts.size();
        mp.indexCount  = (uint32_t)verts.size();
        out.primitives.push_back(mp);
    }

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader/obj] '%s': no drawable triangle geometry\n", path);
        return false;
    }
    std::fprintf(stderr,
        "[model_loader/obj] '%s': %u prims, %zu verts, %zu materials, %zu textures\n",
        path, out.primitiveCount, out.vertices.size(), out.materials.size(), out.textures.size());
    return true;
}
