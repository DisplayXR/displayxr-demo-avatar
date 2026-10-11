// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU ray cast against the loaded model: "ray -> nearest surface".
 *
 * Built once per model load, queried a handful of times per frame (the
 * depth-aware cursor fires ~10-20 rays, XR_DXR_cursor_depth / ADR-046).
 *
 * Shape: one BVH per RIGID CLUSTER, in the cluster's own space, plus one
 * world-from-cluster matrix per cluster that the renderer refreshes when the
 * pose changes. A ray is mapped into each cluster's space by the inverse
 * matrix and traversed there — an affine map preserves the ray parameter t, so
 * the hit's t is the WORLD t with no rescale, and node animation costs nothing
 * (the BVH never rebuilds; only the 3x4 inverse changes).
 *
 *   - A non-skinned primitive is one cluster; its matrix is the primitive's
 *     model matrix (the same push constant pbr.vert uses).
 *   - A skinned primitive is split by DOMINANT JOINT (the joint with the most
 *     summed weight over the triangle's three vertices); each piece's matrix is
 *     that joint's skinning matrix (nodeWorld * inverseBind, the SSBO value).
 *     That is rigid skinning — exact for a triangle bound to one joint, off by
 *     the blend in the seams — which is plenty for a cursor's depth.
 *   - Morph targets are NOT followed: the BVH holds the base shape.
 *
 * Triangles are two-sided (Moller-Trumbore without a cull): the cursor wants
 * the nearest surface, whatever its winding.
 *
 * PROVENANCE: copied verbatim from displayxr-demo-modelviewer's
 * model_common/model_raycast.{h,cpp} (the depth-cursor pilot, modelviewer
 * PR #171 / b2ac9e05c) — this demo's model_common/ is a fork of that one, so
 * ModelVertex / ModelPrimitive match field for field. Keep the two copies in
 * step; a later move into displayxr-common (it needs only those two structs)
 * would dedupe them.
 */

#pragma once

#include <cstdint>
#include <vector>

struct ModelVertex;
struct ModelPrimitive;

class ModelRaycaster {
public:
    //! Build every cluster's BVH from the CPU geometry. `prims` supplies the
    //! index ranges and skin assignment; matrices are set later. Replaces any
    //! previous model.
    void build(const std::vector<ModelVertex>& verts,
               const std::vector<uint32_t>& indices,
               const std::vector<ModelPrimitive>& prims);
    void clear();

    //! Refresh every cluster's world-from-cluster transform: a non-skinned
    //! cluster takes prims[i].modelMatrix, a skinned one takes
    //! jointMats[jointBase + joint] (16 floats per joint, column-major; null or
    //! short = identity, the bind pose the renderer draws without a clip).
    void updateTransforms(const std::vector<ModelPrimitive>& prims,
                          const float* jointMats, uint32_t jointCount);

    //! Nearest hit along origin + t * dir with 0 <= t < tMax. dir need not be
    //! unit; t is in its units. Returns false on a miss.
    bool raycast(const float origin[3], const float dir[3], float& tOut,
                 float tMax = 1e30f) const;

    bool empty() const { return clusters_.empty(); }
    uint32_t triangleCount() const { return (uint32_t)tris_.size(); }
    uint32_t clusterCount() const { return (uint32_t)clusters_.size(); }

private:
    struct Tri {            // Moller-Trumbore form, cluster space
        float v0[3], e1[3], e2[3];
    };
    struct Node {           // 32 bytes
        float bmin[3];
        uint32_t first;     // leaf: first tri; inner: left child (right = left + 1)
        float bmax[3];
        uint32_t count;     // > 0 = leaf
    };
    struct Cluster {
        int      prim = -1;
        int      joint = -1;        // global joint index (skinned), -1 = prim matrix
        uint32_t rootNode = 0;
        uint32_t first = 0, count = 0;  // this cluster's range of tris_
        float    inv[12];           // cluster-from-world, rows of a 3x4
    };

    void buildInto(uint32_t slot, uint32_t first, uint32_t count,
                   std::vector<uint32_t>& order, const std::vector<float>& centroids);

    std::vector<Tri>     tris_;
    std::vector<Node>    nodes_;
    std::vector<Cluster> clusters_;
};
