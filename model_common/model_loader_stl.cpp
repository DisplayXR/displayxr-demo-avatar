// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  STL loader backend (hand-rolled, no dependency) → ModelData.
 *
 * Routed to from model_loader_load() for .stl. STL is geometry-only: a flat
 * triangle soup with no materials, UVs, or scene graph. We emit one primitive
 * with an identity world matrix and a single neutral default material (mid-grey,
 * dielectric, fairly rough) — NOT the renderer's glTF white default, which would
 * read as an unlit-looking flat surface. Per-vertex normals are the stored facet
 * normal, or a computed face normal when the file leaves it zero.
 *
 * Both encodings are supported. Detection is by size, not the leading token:
 * many *binary* STLs also begin with "solid", so we test whether the file size
 * equals the exact binary layout (84 + 50·triCount) and fall back to ASCII.
 */

#include "model_loader_backends.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

struct Vec3 { float x, y, z; };

Vec3 faceNormal(const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 u{ b.x - a.x, b.y - a.y, b.z - a.z };
    Vec3 v{ c.x - a.x, c.y - a.y, c.z - a.z };
    Vec3 n{ u.y * v.z - u.z * v.y,
            u.z * v.x - u.x * v.z,
            u.x * v.y - u.y * v.x };
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-12f) { n.x /= len; n.y /= len; n.z /= len; }
    else              { n = {0.0f, 1.0f, 0.0f}; }
    return n;
}

// Append one triangle (3 verts) to out, using `n` if non-degenerate else a
// computed face normal. Updates the running AABB.
void emitTriangle(ModelData& out, const Vec3& a, const Vec3& b, const Vec3& c, Vec3 n) {
    if (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) < 1e-9f)
        n = faceNormal(a, b, c);
    const Vec3 tri[3] = { a, b, c };
    for (const Vec3& p : tri) {
        ModelVertex v{};
        v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
        v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
        const uint32_t idx = (uint32_t)out.vertices.size();
        out.vertices.push_back(v);
        out.indices.push_back(idx);
        if (!out.hasBBox) {
            out.bboxMin[0] = out.bboxMax[0] = p.x;
            out.bboxMin[1] = out.bboxMax[1] = p.y;
            out.bboxMin[2] = out.bboxMax[2] = p.z;
            out.hasBBox = true;
        } else {
            out.bboxMin[0] = std::min(out.bboxMin[0], p.x); out.bboxMax[0] = std::max(out.bboxMax[0], p.x);
            out.bboxMin[1] = std::min(out.bboxMin[1], p.y); out.bboxMax[1] = std::max(out.bboxMax[1], p.y);
            out.bboxMin[2] = std::min(out.bboxMin[2], p.z); out.bboxMax[2] = std::max(out.bboxMax[2], p.z);
        }
    }
}

bool parseBinary(const std::vector<char>& buf, ModelData& out) {
    uint32_t triCount = 0;
    std::memcpy(&triCount, buf.data() + 80, 4);  // x86/ARM are little-endian, as is STL
    const char* p = buf.data() + 84;
    for (uint32_t i = 0; i < triCount; ++i) {
        float f[12];
        std::memcpy(f, p, 48);  // normal(3) + v0(3) + v1(3) + v2(3)
        p += 50;                // + 2-byte attribute byte count
        emitTriangle(out,
                     { f[3], f[4],  f[5]  },
                     { f[6], f[7],  f[8]  },
                     { f[9], f[10], f[11] },
                     { f[0], f[1],  f[2]  });
    }
    return triCount > 0;
}

bool parseAscii(const std::vector<char>& buf, ModelData& out) {
    std::string text(buf.begin(), buf.end());
    std::istringstream in(text);
    std::string tok;
    Vec3 normal{0, 0, 0};
    Vec3 verts[3];
    int vi = 0;
    while (in >> tok) {
        if (tok == "facet") {
            in >> tok;  // "normal"
            in >> normal.x >> normal.y >> normal.z;
            vi = 0;
        } else if (tok == "vertex") {
            if (vi < 3) { in >> verts[vi].x >> verts[vi].y >> verts[vi].z; ++vi; }
        } else if (tok == "endfacet") {
            if (vi == 3) emitTriangle(out, verts[0], verts[1], verts[2], normal);
        }
    }
    return out.indices.size() >= 3;
}

}  // namespace

bool model_load_stl(const char* path, ModelData& out) {
    if (!path) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "[model_loader/stl] '%s': cannot open\n", path);
        return false;
    }
    const std::streamsize sz = f.tellg();
    if (sz < 15) {  // smaller than the shortest meaningful STL
        std::fprintf(stderr, "[model_loader/stl] '%s': too small (%lld bytes)\n",
                     path, (long long)sz);
        return false;
    }
    f.seekg(0);
    std::vector<char> buf((size_t)sz);
    f.read(buf.data(), sz);

    // Size-based binary detection (robust against "solid"-prefixed binaries).
    bool binary = false;
    if (sz >= 84) {
        uint32_t triCount = 0;
        std::memcpy(&triCount, buf.data() + 80, 4);
        binary = (84ull + (uint64_t)triCount * 50ull == (uint64_t)sz);
    }

    const bool ok = binary ? parseBinary(buf, out) : parseAscii(buf, out);
    if (!ok || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader/stl] '%s': no triangles parsed (%s)\n",
                     path, binary ? "binary" : "ascii");
        return false;
    }

    // One neutral default material — STL has none. Mid-grey dielectric, rough.
    ModelMaterial mm{};
    mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.7f;
    mm.baseColorFactor[3] = 1.0f;
    mm.metallic = 0.0f;
    mm.roughness = 0.8f;
    out.materials.push_back(mm);

    ModelPrimitive mp{};
    mp.firstIndex   = 0;
    mp.indexCount   = (uint32_t)out.indices.size();
    mp.material     = 0;
    mp.node         = -1;
    mp.firstVertex  = 0;
    mp.vertexCount  = (uint32_t)out.vertices.size();
    mp.skin = -1; mp.morph = -1;
    // Identity model matrix (column-major).
    std::memset(mp.modelMatrix, 0, sizeof(mp.modelMatrix));
    mp.modelMatrix[0] = mp.modelMatrix[5] = mp.modelMatrix[10] = mp.modelMatrix[15] = 1.0f;
    out.primitives.push_back(mp);
    out.primitiveCount = 1;

    std::fprintf(stderr, "[model_loader/stl] '%s': %s, %u tris, %zu verts\n",
                 path, binary ? "binary" : "ascii",
                 mp.indexCount / 3, out.vertices.size());
    return true;
}
