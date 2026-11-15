// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  glTF 2.0 loader backend (tinygltf) → ModelData.
 *
 * One of several format backends behind the model_loader_load() dispatcher
 * (see model_loader.cpp). Entry point is model_load_gltf(); the dispatcher
 * routes .glb/.gltf here. This is the single translation unit that defines the
 * tinygltf implementation. Built with NO stb coupling (see model_loader.h for
 * why): the custom image loader below calls the stb already linked from common/.
 */

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE        // supply a custom image loader (below) that calls
#define TINYGLTF_NO_STB_IMAGE_WRITE  // the stb already linked from common/ — avoids a
#include <tiny_gltf.h>               // duplicate stb implementation (and stb-config bugs)
#include "stb_image.h"               // declarations only; impl is in common/d3d11_renderer.cpp

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>

#include "model_loader.h"
#include "model_loader_backends.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>

namespace {

// Custom tinygltf image loader → decode embedded/external images to RGBA8 via
// the stb implementation already linked from common/d3d11_renderer.cpp.
bool LoadImageStb(tinygltf::Image* image, const int /*imageIdx*/, std::string* err,
                  std::string* /*warn*/, int /*reqW*/, int /*reqH*/,
                  const unsigned char* bytes, int size, void* /*user*/) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);  // force RGBA
    if (!data) {
        if (err) *err = std::string("stbi_load_from_memory failed: ") +
                        (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }
    image->width = w;
    image->height = h;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

// Compose a node's local transform: explicit matrix if present, else T*R*S.
glm::mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        return glm::make_mat4(node.matrix.data());  // column-major in glTF too
    }
    glm::mat4 m(1.0f);
    if (node.translation.size() == 3) {
        m = glm::translate(m, glm::vec3((float)node.translation[0],
                                        (float)node.translation[1],
                                        (float)node.translation[2]));
    }
    if (node.rotation.size() == 4) {
        // glTF quaternion is (x, y, z, w); glm::quat ctor is (w, x, y, z).
        glm::quat q((float)node.rotation[3], (float)node.rotation[0],
                    (float)node.rotation[1], (float)node.rotation[2]);
        m = m * glm::mat4_cast(q);
    }
    if (node.scale.size() == 3) {
        m = glm::scale(m, glm::vec3((float)node.scale[0],
                                    (float)node.scale[1],
                                    (float)node.scale[2]));
    }
    return m;
}

// Read accessor element `i`, component `c` as float (handles the float and
// normalized-integer position/normal/uv cases we care about).
const unsigned char* accessorPtr(const tinygltf::Model& m,
                                 const tinygltf::Accessor& acc,
                                 size_t& strideOut) {
    const tinygltf::BufferView& bv = m.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = m.buffers[bv.buffer];
    const int compCount = tinygltf::GetNumComponentsInType(acc.type);
    const int compBytes = tinygltf::GetComponentSizeInBytes(acc.componentType);
    strideOut = bv.byteStride ? bv.byteStride : (size_t)(compCount * compBytes);
    return buf.data.data() + bv.byteOffset + acc.byteOffset;
}

void readVec(const tinygltf::Model& m, int accessorIdx, int comps,
             std::vector<float>& out) {
    out.clear();
    if (accessorIdx < 0) return;
    const tinygltf::Accessor& acc = m.accessors[accessorIdx];
    size_t stride = 0;
    const unsigned char* base = accessorPtr(m, acc, stride);
    out.resize(acc.count * comps);
    for (size_t i = 0; i < acc.count; ++i) {
        const float* f = reinterpret_cast<const float*>(base + i * stride);
        for (int c = 0; c < comps; ++c) out[i * comps + c] = f[c];
    }
}

// Read an accessor as a flat float array (count * numComponents). Returns the
// number of components per element (1 = SCALAR, 3 = VEC3, 4 = VEC4 …) so the
// caller can stride it. Used for animation sampler input/output buffers.
int readAccessorFloats(const tinygltf::Model& m, int accessorIdx,
                       std::vector<float>& out) {
    out.clear();
    if (accessorIdx < 0 || accessorIdx >= (int)m.accessors.size()) return 0;
    const tinygltf::Accessor& acc = m.accessors[accessorIdx];
    const int comps = tinygltf::GetNumComponentsInType(acc.type);
    size_t stride = 0;
    const unsigned char* base = accessorPtr(m, acc, stride);
    out.resize(acc.count * comps);
    for (size_t i = 0; i < acc.count; ++i) {
        const float* f = reinterpret_cast<const float*>(base + i * stride);
        for (int c = 0; c < comps; ++c) out[i * comps + c] = f[c];
    }
    return comps;
}

// Read a JOINTS_0 accessor (VEC4 of UNSIGNED_BYTE or UNSIGNED_SHORT) into a flat
// uint16 array (4 per vertex). glTF guarantees ≤ u16 joint indices.
void readJoints(const tinygltf::Model& m, int accessorIdx,
                std::vector<uint16_t>& out) {
    out.clear();
    if (accessorIdx < 0 || accessorIdx >= (int)m.accessors.size()) return;
    const tinygltf::Accessor& acc = m.accessors[accessorIdx];
    size_t stride = 0;
    const unsigned char* base = accessorPtr(m, acc, stride);
    out.resize(acc.count * 4);
    const bool u8 = acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    for (size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * stride;
        for (int c = 0; c < 4; ++c)
            out[i * 4 + c] = u8 ? (uint16_t)p[c]
                                : *reinterpret_cast<const uint16_t*>(p + c * 2);
    }
}

// Read a WEIGHTS_0 accessor (VEC4) into a flat float array (4 per vertex).
// FLOAT passes through; normalized UNSIGNED_BYTE/SHORT are decoded to [0,1].
void readWeights(const tinygltf::Model& m, int accessorIdx,
                 std::vector<float>& out) {
    out.clear();
    if (accessorIdx < 0 || accessorIdx >= (int)m.accessors.size()) return;
    const tinygltf::Accessor& acc = m.accessors[accessorIdx];
    size_t stride = 0;
    const unsigned char* base = accessorPtr(m, acc, stride);
    out.resize(acc.count * 4);
    for (size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * stride;
        for (int c = 0; c < 4; ++c) {
            switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    out[i * 4 + c] = p[c] / 255.0f; break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    out[i * 4 + c] = reinterpret_cast<const uint16_t*>(p)[c] / 65535.0f; break;
                default:  // FLOAT
                    out[i * 4 + c] = reinterpret_cast<const float*>(p)[c]; break;
            }
        }
    }
}

uint32_t readIndex(const tinygltf::Model& m, const tinygltf::Accessor& acc,
                   const unsigned char* base, size_t stride, size_t i) {
    const unsigned char* p = base + i * stride;
    switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return *p;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
        default: return 0;
    }
}

// ── Scene-graph and index validation (#125) ─────────────────────────────────
// The readers above trust every index and byte range the file declares. The
// checks below run before them, so a malformed file degrades (a dropped edge, a
// skipped primitive, a default material) instead of reading out of bounds or
// recursing without end. They check ranges only; they decode nothing.

// Deepest node chain kept. Real skeletons are tens of levels deep; the cap only
// stops a pathological chain from exhausting the stack in processNode().
constexpr int kMaxNodeDepth = 256;

// Index/range problems that are repaired in place rather than skipping a
// primitive. Logged once, as one summary line, at the end of the load.
struct IndexIssues {
    size_t badMesh = 0;         // node.mesh out of range -> node draws nothing
    size_t badSkin = 0;         // node.skin out of range -> primitives unskinned
    size_t badMaterial = 0;     // primitive.material out of range -> default material
    size_t badJoint = 0;        // skin.joints[] entry out of range -> -1 (identity)
    size_t badInverseBind = 0;  // inverseBindMatrices unreadable -> identity
};

// `idx` as an accessor of `type` with one of `compTypes`, whose bufferView and
// buffer exist and whose last element (strided the way accessorPtr() strides
// it) ends inside both; nullptr otherwise.
const tinygltf::Accessor* checkedAccessor(const tinygltf::Model& m, int idx, int type,
                                          std::initializer_list<int> compTypes) {
    if (idx < 0 || idx >= (int)m.accessors.size()) return nullptr;
    const tinygltf::Accessor& acc = m.accessors[(size_t)idx];
    if (acc.type != type) return nullptr;
    if (std::find(compTypes.begin(), compTypes.end(), acc.componentType) == compTypes.end())
        return nullptr;
    // accessorPtr() dereferences the bufferView even for an empty accessor.
    if (acc.bufferView < 0 || acc.bufferView >= (int)m.bufferViews.size()) return nullptr;
    const tinygltf::BufferView& bv = m.bufferViews[(size_t)acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= (int)m.buffers.size()) return nullptr;
    const size_t bufSize = m.buffers[(size_t)bv.buffer].data.size();
    if (bv.byteOffset > bufSize || bv.byteLength > bufSize - bv.byteOffset) return nullptr;
    if (acc.count == 0) return &acc;
    const size_t elem =
        (size_t)tinygltf::GetNumComponentsInType((uint32_t)acc.type) *
        (size_t)tinygltf::GetComponentSizeInBytes((uint32_t)acc.componentType);
    const size_t stride = bv.byteStride ? bv.byteStride : elem;
    if (acc.byteOffset > bv.byteLength) return nullptr;
    const size_t avail = bv.byteLength - acc.byteOffset;
    if (elem > avail || (acc.count - 1) > (avail - elem) / stride) return nullptr;
    return &acc;
}

// Why `prim` cannot be read safely, or nullptr when it can. `skinned` = the
// primitive will be read as skinned (valid node.skin + JOINTS_0 + WEIGHTS_0).
const char* primitiveProblem(const tinygltf::Model& m, const tinygltf::Primitive& prim,
                             bool skinned) {
    constexpr int kF  = TINYGLTF_COMPONENT_TYPE_FLOAT;
    constexpr int kUB = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    constexpr int kUS = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    constexpr int kUI = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;

    auto itPos = prim.attributes.find("POSITION");
    const tinygltf::Accessor* pos = itPos == prim.attributes.end() ? nullptr
        : checkedAccessor(m, itPos->second, TINYGLTF_TYPE_VEC3, {kF});
    if (!pos) return "POSITION accessor is out of range or malformed";
    const size_t vcount = pos->count;

    // An optional attribute must, when present, be readable and per-vertex.
    auto attrOk = [&](const char* name, int type, std::initializer_list<int> comps) {
        auto it = prim.attributes.find(name);
        if (it == prim.attributes.end()) return true;
        const tinygltf::Accessor* a = checkedAccessor(m, it->second, type, comps);
        return a != nullptr && a->count == vcount;
    };
    if (!attrOk("NORMAL", TINYGLTF_TYPE_VEC3, {kF}))
        return "NORMAL accessor is out of range, malformed or not per-vertex";
    if (!attrOk("TEXCOORD_0", TINYGLTF_TYPE_VEC2, {kF}))
        return "TEXCOORD_0 accessor is out of range, malformed or not per-vertex";
    if (skinned) {
        if (!attrOk("JOINTS_0", TINYGLTF_TYPE_VEC4, {kUB, kUS}))
            return "JOINTS_0 accessor is out of range, malformed or not per-vertex";
        if (!attrOk("WEIGHTS_0", TINYGLTF_TYPE_VEC4, {kF, kUB, kUS}))
            return "WEIGHTS_0 accessor is out of range, malformed or not per-vertex";
    }

    for (const auto& tgt : prim.targets) {
        for (const char* name : {"POSITION", "NORMAL"}) {
            auto it = tgt.find(name);
            if (it != tgt.end() && !checkedAccessor(m, it->second, TINYGLTF_TYPE_VEC3, {kF}))
                return "morph-target accessor is out of range or malformed";
        }
    }

    if (prim.indices >= 0) {
        const tinygltf::Accessor* ia =
            checkedAccessor(m, prim.indices, TINYGLTF_TYPE_SCALAR, {kUB, kUS, kUI});
        if (!ia) return "indices accessor is out of range or malformed";
        size_t istride = 0;
        const unsigned char* ibase = accessorPtr(m, *ia, istride);
        for (size_t i = 0; i < ia->count; ++i)
            if (readIndex(m, *ia, ibase, istride, i) >= vcount)
                return "an index names a vertex outside the primitive";
    }
    return nullptr;
}

// Turns the file's node graph into a tree before anything walks it: every
// child index in range, at most one parent per node, no edge closing a cycle,
// no chain deeper than kMaxNodeDepth. `kids` = the surviving child lists
// (1:1 with m.nodes, file order kept); `roots` = the valid, distinct nodes of
// scene `sceneIdx`, or, when there is no such scene, every node no surviving
// edge points at. Dropped roots and edges are counted into one log line.
void validateNodeGraph(const tinygltf::Model& m, int sceneIdx,
                       std::vector<std::vector<int>>& kids, std::vector<int>& roots) {
    const int n = (int)m.nodes.size();
    kids.assign((size_t)n, {});
    roots.clear();

    std::vector<int> candidates;
    const bool haveScene = sceneIdx >= 0 && sceneIdx < (int)m.scenes.size();
    if (haveScene) {
        candidates = m.scenes[(size_t)sceneIdx].nodes;
    } else {
        std::vector<char> pointedAt((size_t)n, 0);
        for (const tinygltf::Node& node : m.nodes)
            for (int c : node.children)
                if (c >= 0 && c < n) pointedAt[(size_t)c] = 1;
        for (int i = 0; i < n; ++i)
            if (!pointedAt[(size_t)i]) candidates.push_back(i);
    }

    std::vector<char> reached((size_t)n, 0);
    std::vector<std::pair<int, int>> stack;  // (node, depth)
    size_t badRoots = 0, badEdges = 0;
    for (int r : candidates) {
        if (r < 0 || r >= n || reached[(size_t)r]) { ++badRoots; continue; }
        reached[(size_t)r] = 1;
        roots.push_back(r);
        stack.assign(1, {r, 0});
        while (!stack.empty()) {
            const auto [p, depth] = stack.back();
            stack.pop_back();
            for (int c : m.nodes[(size_t)p].children) {
                if (c < 0 || c >= n || reached[(size_t)c] || depth + 1 > kMaxNodeDepth) {
                    ++badEdges;
                    continue;
                }
                reached[(size_t)c] = 1;
                kids[(size_t)p].push_back(c);
                stack.push_back({c, depth + 1});
            }
        }
    }
    size_t unreached = 0;
    if (!haveScene)
        for (int i = 0; i < n; ++i) if (!reached[(size_t)i]) ++unreached;

    if (!haveScene && !m.scenes.empty())
        std::fprintf(stderr,
                     "[model_loader] scene %d does not exist (%zu scene(s)); walking every "
                     "parentless node instead\n", sceneIdx, m.scenes.size());
    if (badRoots || badEdges || unreached)
        std::fprintf(stderr,
                     "[model_loader] node graph repaired: dropped %zu scene root(s) and %zu "
                     "child edge(s) (index out of range, second parent, cycle, or deeper than "
                     "%d); %zu node(s) unreachable\n",
                     badRoots, badEdges, kMaxNodeDepth, unreached);
}

void processNode(const tinygltf::Model& m, int nodeIdx, const glm::mat4& parent,
                 const std::vector<std::vector<int>>& kids, IndexIssues& issues,
                 ModelData& out) {
    const tinygltf::Node& node = m.nodes[nodeIdx];
    const glm::mat4 world = parent * nodeLocalMatrix(node);

    if (node.mesh < -1 || node.mesh >= (int)m.meshes.size()) ++issues.badMesh;
    const bool skinValid = node.skin >= 0 && node.skin < (int)m.skins.size();
    if (node.skin < -1 || node.skin >= (int)m.skins.size()) ++issues.badSkin;
    if (node.mesh >= 0 && node.mesh < (int)m.meshes.size()) {
        const tinygltf::Mesh& mesh = m.meshes[node.mesh];
        for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx) {
            const tinygltf::Primitive& prim = mesh.primitives[primIdx];
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;

            auto itPos = prim.attributes.find("POSITION");
            if (itPos == prim.attributes.end()) continue;
            auto itNrm = prim.attributes.find("NORMAL");
            auto itUv  = prim.attributes.find("TEXCOORD_0");
            auto itJnt = prim.attributes.find("JOINTS_0");
            auto itWgt = prim.attributes.find("WEIGHTS_0");

            // A primitive is skinned only when the node has a (valid) skin AND
            // carries both joint + weight attributes.
            const bool skinned = skinValid &&
                                 itJnt != prim.attributes.end() &&
                                 itWgt != prim.attributes.end();
            if (const char* why = primitiveProblem(m, prim, skinned)) {
                std::fprintf(stderr, "[model_loader] mesh %d primitive %zu skipped: %s\n",
                             node.mesh, primIdx, why);
                continue;
            }

            std::vector<float> pos, nrm, uv, wgt;
            std::vector<uint16_t> jnt;
            readVec(m, itPos->second, 3, pos);
            readVec(m, itNrm != prim.attributes.end() ? itNrm->second : -1, 3, nrm);
            readVec(m, itUv  != prim.attributes.end() ? itUv->second  : -1, 2, uv);
            if (skinned) {
                readJoints(m, itJnt->second, jnt);
                readWeights(m, itWgt->second, wgt);
                // Every joint a vertex names must exist in the skin: the joint
                // matrices are indexed by jointBase + j on the GPU and on the CPU.
                const size_t nj = m.skins[(size_t)node.skin].joints.size();
                if (std::any_of(jnt.begin(), jnt.end(),
                                [nj](uint16_t j) { return (size_t)j >= nj; })) {
                    std::fprintf(stderr,
                                 "[model_loader] mesh %d primitive %zu skipped: JOINTS_0 "
                                 "names a joint skin %d does not have (%zu joints)\n",
                                 node.mesh, primIdx, node.skin, nj);
                    continue;
                }
            }

            const size_t vcount = pos.size() / 3;
            const uint32_t vertexBase = (uint32_t)out.vertices.size();

            for (size_t i = 0; i < vcount; ++i) {
                ModelVertex v{};
                v.pos[0] = pos[i * 3 + 0]; v.pos[1] = pos[i * 3 + 1]; v.pos[2] = pos[i * 3 + 2];
                if (!nrm.empty()) { v.normal[0] = nrm[i*3+0]; v.normal[1] = nrm[i*3+1]; v.normal[2] = nrm[i*3+2]; }
                else              { v.normal[0] = 0; v.normal[1] = 1; v.normal[2] = 0; }
                if (!uv.empty())  { v.uv[0] = uv[i*2+0]; v.uv[1] = uv[i*2+1]; }
                if (skinned && jnt.size() >= (i + 1) * 4 && wgt.size() >= (i + 1) * 4) {
                    for (int c = 0; c < 4; ++c) {
                        v.joints0[c]  = jnt[i * 4 + c];
                        v.weights0[c] = wgt[i * 4 + c];
                    }
                }
                out.vertices.push_back(v);

                // AABB. Skinned meshes are placed purely by their joint matrices
                // (the mesh node transform is ignored, per glTF), and in bind
                // pose that resolves to the raw POSITION — so accumulate skinned
                // verts untransformed; static verts use the baked world matrix.
                glm::vec4 wp = skinned ? glm::vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f)
                                       : world * glm::vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f);
                if (!out.hasBBox) {
                    out.bboxMin[0] = out.bboxMax[0] = wp.x;
                    out.bboxMin[1] = out.bboxMax[1] = wp.y;
                    out.bboxMin[2] = out.bboxMax[2] = wp.z;
                    out.hasBBox = true;
                } else {
                    out.bboxMin[0] = std::min(out.bboxMin[0], wp.x); out.bboxMax[0] = std::max(out.bboxMax[0], wp.x);
                    out.bboxMin[1] = std::min(out.bboxMin[1], wp.y); out.bboxMax[1] = std::max(out.bboxMax[1], wp.y);
                    out.bboxMin[2] = std::min(out.bboxMin[2], wp.z); out.bboxMax[2] = std::max(out.bboxMax[2], wp.z);
                }
            }

            // Morph targets: flatten each target's POSITION/NORMAL deltas into a
            // ModelMorph laid out [target][vertex][xyz]. The renderer blends
            // base + Σ weightᵢ·deltaᵢ per frame; weights live on the owning node.
            int morphIdx = -1;
            if (!prim.targets.empty()) {
                ModelMorph mm;
                mm.targetCount = (uint32_t)prim.targets.size();
                mm.vertexCount = (uint32_t)vcount;
                mm.posDeltas.assign((size_t)mm.targetCount * vcount * 3, 0.0f);
                bool anyNrm = false;
                for (const auto& tgt : prim.targets) if (tgt.count("NORMAL")) { anyNrm = true; break; }
                if (anyNrm) mm.nrmDeltas.assign((size_t)mm.targetCount * vcount * 3, 0.0f);
                std::vector<float> tmp;
                for (uint32_t t = 0; t < mm.targetCount; ++t) {
                    const auto& tgt = prim.targets[t];
                    const size_t off = (size_t)t * vcount * 3;
                    auto pit = tgt.find("POSITION");
                    if (pit != tgt.end()) {
                        readVec(m, pit->second, 3, tmp);
                        std::copy_n(tmp.begin(), std::min(tmp.size(), (size_t)vcount * 3),
                                    mm.posDeltas.begin() + off);
                    }
                    auto nit = tgt.find("NORMAL");
                    if (anyNrm && nit != tgt.end()) {
                        readVec(m, nit->second, 3, tmp);
                        std::copy_n(tmp.begin(), std::min(tmp.size(), (size_t)vcount * 3),
                                    mm.nrmDeltas.begin() + off);
                    }
                }
                morphIdx = (int)out.morphs.size();
                out.morphs.push_back(std::move(mm));
            }

            ModelPrimitive mp{};
            mp.firstIndex = (uint32_t)out.indices.size();
            mp.material = prim.material;
            if (mp.material < -1 || mp.material >= (int)m.materials.size()) {
                ++issues.badMaterial;
                mp.material = -1;  // renderer default material
            }
            mp.node = nodeIdx;   // owning node → animated world matrix per frame
            mp.skin = skinned ? node.skin : -1;  // jointBase filled in after skins parse
            mp.firstVertex = vertexBase;
            mp.vertexCount = (uint32_t)vcount;
            mp.morph = morphIdx;
            std::memcpy(mp.modelMatrix, glm::value_ptr(world), 16 * sizeof(float));

            if (prim.indices >= 0) {
                const tinygltf::Accessor& iacc = m.accessors[prim.indices];
                size_t istride = 0;
                const unsigned char* ibase = accessorPtr(m, iacc, istride);
                if (istride == 0) istride = tinygltf::GetComponentSizeInBytes(iacc.componentType);
                for (size_t i = 0; i < iacc.count; ++i)
                    out.indices.push_back(vertexBase + readIndex(m, iacc, ibase, istride, i));
                mp.indexCount = (uint32_t)iacc.count;
            } else {
                // Non-indexed: emit a sequential index range.
                for (size_t i = 0; i < vcount; ++i)
                    out.indices.push_back(vertexBase + (uint32_t)i);
                mp.indexCount = (uint32_t)vcount;
            }

            if (mp.indexCount > 0) out.primitives.push_back(mp);
        }
    }

    // `kids` is the validated tree (validateNodeGraph), not node.children.
    for (int child : kids[(size_t)nodeIdx]) processNode(m, child, world, kids, issues, out);
}

}  // namespace

bool model_load_gltf(const char* gltfPath, ModelData& out) {
    if (!gltfPath) return false;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&LoadImageStb, nullptr);
    std::string err, warn;

    const std::string path = gltfPath;
    const bool isBinary =
        path.size() >= 4 &&
        (std::strcmp(path.c_str() + path.size() - 4, ".glb") == 0 ||
         std::strcmp(path.c_str() + path.size() - 4, ".GLB") == 0);

    const bool ok = isBinary
        ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
        : loader.LoadASCIIFromFile(&model, &err, &warn, path);

    if (!warn.empty()) std::fprintf(stderr, "[model_loader] warn: %s\n", warn.c_str());
    if (!ok) {
        std::fprintf(stderr, "[model_loader] error: %s\n",
                     err.empty() ? "unknown parse error" : err.c_str());
        return false;
    }

    // Decode images → RGBA8 (parallel to model.images). Empty entries (decode
    // failed / unsupported bit depth) make the renderer fall back to a default.
    out.textures.resize(model.images.size());
    for (size_t i = 0; i < model.images.size(); ++i) {
        const tinygltf::Image& img = model.images[i];
        if (img.image.empty() || img.width <= 0 || img.height <= 0 ||
            img.bits != 8 || img.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            std::fprintf(stderr, "[model_loader] image %zu skipped (w=%d h=%d bits=%d comp=%d)\n",
                         i, img.width, img.height, img.bits, img.component);
            continue;
        }
        ModelTexture t;
        t.width = img.width;
        t.height = img.height;
        t.rgba.resize((size_t)img.width * img.height * 4);
        const int comp = img.component;
        const unsigned char* s = img.image.data();
        for (size_t p = 0; p < (size_t)img.width * img.height; ++p) {
            uint8_t r = s[p * comp + 0];
            uint8_t g = (comp >= 3) ? s[p * comp + 1] : r;       // 1/2-comp → grayscale
            uint8_t b = (comp >= 3) ? s[p * comp + 2] : r;
            uint8_t a = (comp == 4) ? s[p * comp + 3]
                       : (comp == 2) ? s[p * comp + 1] : 255;
            t.rgba[p * 4 + 0] = r; t.rgba[p * 4 + 1] = g;
            t.rgba[p * 4 + 2] = b; t.rgba[p * 4 + 3] = a;
        }
        out.textures[i] = std::move(t);
    }

    // glTF texture index → source image index (== ModelData::textures index).
    auto resolveTex = [&](int gltfTexIndex) -> int {
        if (gltfTexIndex < 0 || gltfTexIndex >= (int)model.textures.size()) return -1;
        int src = model.textures[gltfTexIndex].source;
        if (src < 0 || src >= (int)out.textures.size()) return -1;
        return out.textures[src].rgba.empty() ? -1 : src;  // -1 → renderer default
    };

    // Materials (factors + texture refs).
    out.materials.reserve(model.materials.size());
    for (const auto& mat : model.materials) {
        ModelMaterial mm{};
        const auto& pbr = mat.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4)
            for (int i = 0; i < 4; ++i) mm.baseColorFactor[i] = (float)pbr.baseColorFactor[i];
        mm.metallic = (float)pbr.metallicFactor;
        mm.roughness = (float)pbr.roughnessFactor;
        if (mat.emissiveFactor.size() == 3)
            for (int i = 0; i < 3; ++i) mm.emissive[i] = (float)mat.emissiveFactor[i];
        mm.baseColorTex          = resolveTex(pbr.baseColorTexture.index);
        mm.metallicRoughnessTex  = resolveTex(pbr.metallicRoughnessTexture.index);
        mm.normalTex             = resolveTex(mat.normalTexture.index);
        mm.occlusionTex          = resolveTex(mat.occlusionTexture.index);
        mm.emissiveTex           = resolveTex(mat.emissiveTexture.index);
        out.materials.push_back(mm);
    }

    // Walk the default scene (or scene 0) over the validated tree: `kids`
    // replaces node.children everywhere below (bake walk + retained graph), so
    // neither this walk nor the renderer's per-frame DFS can meet a cycle.
    const int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
    const glm::mat4 ident(1.0f);
    std::vector<std::vector<int>> kids;
    std::vector<int> roots;
    validateNodeGraph(model, sceneIdx, kids, roots);
    IndexIssues issues;
    for (int n : roots) processNode(model, n, ident, kids, issues, out);

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader] '%s' has no drawable triangle geometry\n", gltfPath);
        return false;
    }

    // ── Retain the node graph (1:1 with model.nodes) for per-frame animation ──
    // mp.node indices set in processNode reference these entries directly.
    out.nodes.resize(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const tinygltf::Node& src = model.nodes[i];
        ModelNode& dst = out.nodes[i];
        dst.mesh = src.mesh;
        dst.children = kids[i];
        for (int c : kids[i]) out.nodes[(size_t)c].parent = (int)i;
        if (src.matrix.size() == 16) {
            dst.hasMatrix = true;
            for (int k = 0; k < 16; ++k) dst.matrix[k] = (float)src.matrix[k];
        }
        if (src.translation.size() == 3)
            for (int k = 0; k < 3; ++k) dst.translation[k] = (float)src.translation[k];
        if (src.rotation.size() == 4)
            for (int k = 0; k < 4; ++k) dst.rotation[k] = (float)src.rotation[k];  // xyzw
        if (src.scale.size() == 3)
            for (int k = 0; k < 3; ++k) dst.scale[k] = (float)src.scale[k];
        // Seed morph weights (size = the node's mesh's morph-target count) from
        // the node override, else the mesh default, else zeros. A Weights anim
        // channel overwrites these per frame.
        if (src.mesh >= 0 && src.mesh < (int)model.meshes.size()) {
            const tinygltf::Mesh& msh = model.meshes[src.mesh];
            size_t nTargets = msh.primitives.empty() ? 0 : msh.primitives[0].targets.size();
            if (nTargets > 0) {
                dst.weights.assign(nTargets, 0.0f);
                const std::vector<double>& defs =
                    !src.weights.empty() ? src.weights : msh.weights;
                for (size_t k = 0; k < nTargets && k < defs.size(); ++k)
                    dst.weights[k] = (float)defs[k];
            }
        }
    }
    // Scene roots (same validated selection the world-bake walk used above).
    out.rootNodes = roots;

    // ── Parse skins[] (joint lists + inverse-bind matrices) ───────────────────
    // Joint matrices for every skin are packed back-to-back; jointBase is each
    // skin's offset into that flat array. A skin's inverseBindMatrices accessor
    // may be absent → default each joint to identity (glTF allows this).
    out.skins.resize(model.skins.size());
    uint32_t jointBase = 0;
    for (size_t s = 0; s < model.skins.size(); ++s) {
        const tinygltf::Skin& src = model.skins[s];
        ModelSkin& dst = out.skins[s];
        dst.joints.assign(src.joints.begin(), src.joints.end());
        for (int& j : dst.joints)
            if (j < 0 || j >= (int)model.nodes.size()) { ++issues.badJoint; j = -1; }
        const size_t nj = dst.joints.size();
        if (src.inverseBindMatrices >= 0) {
            if (checkedAccessor(model, src.inverseBindMatrices, TINYGLTF_TYPE_MAT4,
                                {TINYGLTF_COMPONENT_TYPE_FLOAT}))
                readAccessorFloats(model, src.inverseBindMatrices, dst.inverseBind);
            else
                ++issues.badInverseBind;
        }
        if (dst.inverseBind.size() != nj * 16) {  // absent / malformed → identity
            dst.inverseBind.assign(nj * 16, 0.0f);
            for (size_t j = 0; j < nj; ++j)
                dst.inverseBind[j * 16 + 0] = dst.inverseBind[j * 16 + 5] =
                dst.inverseBind[j * 16 + 10] = dst.inverseBind[j * 16 + 15] = 1.0f;
        }
        jointBase += (uint32_t)nj;
    }
    out.totalJoints = jointBase;
    // Back-fill each skinned primitive's jointBase from its skin index.
    {
        std::vector<uint32_t> baseOf(out.skins.size(), 0);
        uint32_t acc = 0;
        for (size_t s = 0; s < out.skins.size(); ++s) {
            baseOf[s] = acc;
            acc += (uint32_t)out.skins[s].joints.size();
        }
        for (ModelPrimitive& p : out.primitives)
            if (p.skin >= 0 && p.skin < (int)baseOf.size())
                p.jointBase = (int)baseOf[p.skin];
    }

    // ── Parse animations[] (channels + samplers) ─────────────────────────────
    auto mapPath = [](const std::string& p, AnimPath& out) -> bool {
        if (p == "translation") { out = AnimPath::Translation; return true; }
        if (p == "rotation")    { out = AnimPath::Rotation;    return true; }
        if (p == "scale")       { out = AnimPath::Scale;       return true; }
        if (p == "weights")     { out = AnimPath::Weights;     return true; }
        return false;
    };
    for (const auto& src : model.animations) {
        Animation anim;
        anim.name = src.name;
        anim.samplers.reserve(src.samplers.size());
        for (const auto& s : src.samplers) {
            AnimSampler smp;
            readAccessorFloats(model, s.input, smp.input);
            readAccessorFloats(model, s.output, smp.output);
            smp.interp = s.interpolation == "STEP"        ? AnimInterp::Step
                       : s.interpolation == "CUBICSPLINE" ? AnimInterp::CubicSpline
                                                          : AnimInterp::Linear;
            if (!smp.input.empty())
                anim.duration = std::max(anim.duration, smp.input.back());
            anim.samplers.push_back(std::move(smp));
        }
        for (const auto& c : src.channels) {
            AnimChannel ch;
            ch.targetNode = c.target_node;
            ch.sampler = c.sampler;
            if (ch.targetNode < 0 || ch.targetNode >= (int)out.nodes.size()) continue;
            if (ch.sampler < 0 || ch.sampler >= (int)anim.samplers.size()) continue;
            if (!mapPath(c.target_path, ch.path)) continue;
            anim.channels.push_back(ch);
        }
        if (!anim.channels.empty()) out.animations.push_back(std::move(anim));
    }

    if (issues.badMesh || issues.badSkin || issues.badMaterial || issues.badJoint ||
        issues.badInverseBind)
        std::fprintf(stderr,
                     "[model_loader] out-of-range indices repaired: %zu node mesh, %zu node "
                     "skin (unskinned), %zu primitive material (default), %zu skin joint "
                     "(identity), %zu inverseBindMatrices (identity)\n",
                     issues.badMesh, issues.badSkin, issues.badMaterial, issues.badJoint,
                     issues.badInverseBind);

    std::fprintf(stderr,
        "[model_loader] '%s': %u prims, %zu verts, %zu indices, %zu materials, "
        "%zu nodes, %zu animations\n",
        gltfPath, out.primitiveCount, out.vertices.size(), out.indices.size(),
        out.materials.size(), out.nodes.size(), out.animations.size());
    return true;
}
