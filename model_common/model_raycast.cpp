// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU ray cast against the loaded model (see model_raycast.h).
 */

#include "model_raycast.h"
#include "model_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace {

constexpr uint32_t kLeafTris = 4;

inline void tri_centroid(const float v0[3], const float e1[3], const float e2[3], float c[3]) {
    for (int a = 0; a < 3; ++a) c[a] = v0[a] + (e1[a] + e2[a]) * (1.0f / 3.0f);
}

// Inverse of an affine column-major 4x4 as the rows of a 3x4. Singular -> false.
bool affine_inverse(const float m[16], float out[12]) {
    // R[r][c] = m[c*4 + r]
    const float a = m[0], b = m[4], c = m[8];
    const float d = m[1], e = m[5], f = m[9];
    const float g = m[2], h = m[6], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-30f)) return false;
    const float s = 1.0f / det;
    const float r[9] = {A * s, -(b * i - c * h) * s, (b * f - c * e) * s,
                        B * s, (a * i - c * g) * s, -(a * f - c * d) * s,
                        C * s, -(a * h - b * g) * s, (a * e - b * d) * s};
    const float t[3] = {m[12], m[13], m[14]};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0];
        out[row * 4 + 1] = r[row * 3 + 1];
        out[row * 4 + 2] = r[row * 3 + 2];
        out[row * 4 + 3] = -(r[row * 3 + 0] * t[0] + r[row * 3 + 1] * t[1] + r[row * 3 + 2] * t[2]);
    }
    return true;
}

void set_identity(float out[12]) {
    static const float I[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(out, I, sizeof(I));
}

// Slab test; returns the entry t (clamped at 0) or a negative value on a miss.
inline float ray_aabb(const float o[3], const float invD[3], const float bmin[3],
                      const float bmax[3], float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        float ta = (bmin[a] - o[a]) * invD[a];
        float tb = (bmax[a] - o[a]) * invD[a];
        if (ta > tb) std::swap(ta, tb);
        // NaN (0 * inf on a slab boundary) must not shrink the interval.
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

} // namespace

void ModelRaycaster::clear() {
    tris_.clear();
    nodes_.clear();
    clusters_.clear();
}

void ModelRaycaster::buildInto(uint32_t slot, uint32_t first, uint32_t count,
                               std::vector<uint32_t>& order, const std::vector<float>& c) {
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    float cmin[3] = {1e30f, 1e30f, 1e30f}, cmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t k = first; k < first + count; ++k) {
        const uint32_t ti = order[k];
        const Tri& t = tris_[ti];
        for (int a = 0; a < 3; ++a) {
            const float p0 = t.v0[a], p1 = t.v0[a] + t.e1[a], p2 = t.v0[a] + t.e2[a];
            bmin[a] = std::min(bmin[a], std::min(p0, std::min(p1, p2)));
            bmax[a] = std::max(bmax[a], std::max(p0, std::max(p1, p2)));
            cmin[a] = std::min(cmin[a], c[ti * 3 + a]);
            cmax[a] = std::max(cmax[a], c[ti * 3 + a]);
        }
    }
    Node n{};
    std::memcpy(n.bmin, bmin, sizeof(bmin));
    std::memcpy(n.bmax, bmax, sizeof(bmax));
    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if (cmax[a] - cmin[a] > cmax[axis] - cmin[axis]) axis = a;
    if (count <= kLeafTris || !(cmax[axis] - cmin[axis] > 0.0f)) {
        n.first = first;          // a range of `order`; build() applies it to tris_
        n.count = count;
        nodes_[slot] = n;
        return;
    }
    // Median split on the centroid along the widest centroid axis.
    const uint32_t half = count / 2;
    std::nth_element(order.begin() + first, order.begin() + first + half, order.begin() + first + count,
                     [&](uint32_t x, uint32_t y) { return c[x * 3 + axis] < c[y * 3 + axis]; });
    // Children are adjacent (right = left + 1).
    const uint32_t left = (uint32_t)nodes_.size();
    nodes_.push_back(Node{});
    nodes_.push_back(Node{});
    n.first = left;
    n.count = 0;
    nodes_[slot] = n;
    buildInto(left, first, half, order, c);
    buildInto(left + 1, first + half, count - half, order, c);
}

void ModelRaycaster::build(const std::vector<ModelVertex>& verts,
                           const std::vector<uint32_t>& indices,
                           const std::vector<ModelPrimitive>& prims) {
    clear();
    // Gather triangles per cluster key (prim, joint), in cluster space (= the
    // vertex buffer's space: mesh-local, or skin space for skinned prims).
    std::map<std::pair<int, int>, std::vector<Tri>> buckets;
    for (size_t pi = 0; pi < prims.size(); ++pi) {
        const ModelPrimitive& p = prims[pi];
        for (uint32_t k = 0; k + 2 < p.indexCount; k += 3) {
            const uint32_t i0 = indices[p.firstIndex + k], i1 = indices[p.firstIndex + k + 1],
                           i2 = indices[p.firstIndex + k + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;
            const ModelVertex* v[3] = {&verts[i0], &verts[i1], &verts[i2]};
            int joint = -1;
            if (p.skin >= 0) {
                // Dominant joint by summed weight over the three vertices.
                float best = -1.0f;
                for (int a = 0; a < 3; ++a)
                    for (int s = 0; s < 4; ++s) {
                        const int j = v[a]->joints0[s];
                        float w = 0.0f;
                        for (int b = 0; b < 3; ++b)
                            for (int u = 0; u < 4; ++u)
                                if (v[b]->joints0[u] == j) w += v[b]->weights0[u];
                        if (w > best) { best = w; joint = p.jointBase + j; }
                    }
            }
            Tri t;
            for (int a = 0; a < 3; ++a) {
                t.v0[a] = v[0]->pos[a];
                t.e1[a] = v[1]->pos[a] - v[0]->pos[a];
                t.e2[a] = v[2]->pos[a] - v[0]->pos[a];
            }
            buckets[{(int)pi, joint}].push_back(t);
        }
    }
    size_t total = 0;
    for (auto& kv : buckets) total += kv.second.size();
    tris_.reserve(total);
    nodes_.reserve(total / kLeafTris * 2 + buckets.size() * 2);
    for (auto& kv : buckets) {
        const uint32_t first = (uint32_t)tris_.size(), count = (uint32_t)kv.second.size();
        tris_.insert(tris_.end(), kv.second.begin(), kv.second.end());
        kv.second.clear(); kv.second.shrink_to_fit();
        Cluster cl;
        cl.prim = kv.first.first;
        cl.joint = kv.first.second;
        cl.first = first;
        cl.count = count;
        set_identity(cl.inv);
        clusters_.push_back(cl);
    }
    std::vector<float> cent(tris_.size() * 3);
    for (size_t k = 0; k < tris_.size(); ++k)
        tri_centroid(tris_[k].v0, tris_[k].e1, tris_[k].e2, &cent[k * 3]);
    std::vector<uint32_t> order(tris_.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = (uint32_t)k;
    for (Cluster& cl : clusters_) {
        cl.rootNode = (uint32_t)nodes_.size();
        nodes_.push_back(Node{});
        buildInto(cl.rootNode, cl.first, cl.count, order, cent);
    }
    // Leaves index ranges of `order`; make them ranges of tris_ directly.
    std::vector<Tri> sorted(tris_.size());
    for (size_t k = 0; k < order.size(); ++k) sorted[k] = tris_[order[k]];
    tris_.swap(sorted);
}

void ModelRaycaster::updateTransforms(const std::vector<ModelPrimitive>& prims,
                                      const float* jointMats, uint32_t jointCount) {
    for (Cluster& cl : clusters_) {
        const float* m = nullptr;
        if (cl.joint >= 0) {
            if (jointMats && (uint32_t)cl.joint < jointCount) m = jointMats + (size_t)cl.joint * 16;
        } else if (cl.prim >= 0 && (size_t)cl.prim < prims.size()) {
            m = prims[cl.prim].modelMatrix;
        }
        if (!m || !affine_inverse(m, cl.inv)) set_identity(cl.inv);
    }
}

bool ModelRaycaster::raycast(const float origin[3], const float dir[3], float& tOut,
                             float tMax) const {
    float best = tMax;
    bool hit = false;
    uint32_t stack[64];
    for (const Cluster& cl : clusters_) {
        const float* M = cl.inv;
        float o[3], d[3], invD[3];
        for (int r = 0; r < 3; ++r) {
            o[r] = M[r * 4 + 0] * origin[0] + M[r * 4 + 1] * origin[1] + M[r * 4 + 2] * origin[2] + M[r * 4 + 3];
            d[r] = M[r * 4 + 0] * dir[0] + M[r * 4 + 1] * dir[1] + M[r * 4 + 2] * dir[2];
            invD[r] = 1.0f / d[r];   // +-inf on an axis-parallel ray: the slab test handles it
        }
        uint32_t sp = 0;
        stack[sp++] = cl.rootNode;
        while (sp > 0) {
            const Node& n = nodes_[stack[--sp]];
            if (ray_aabb(o, invD, n.bmin, n.bmax, best) < 0.0f) continue;
            if (n.count > 0) {
                for (uint32_t k = n.first; k < n.first + n.count; ++k) {
                    const Tri& t = tris_[k];
                    // Moller-Trumbore, two-sided.
                    const float p[3] = {d[1] * t.e2[2] - d[2] * t.e2[1], d[2] * t.e2[0] - d[0] * t.e2[2],
                                        d[0] * t.e2[1] - d[1] * t.e2[0]};
                    const float det = t.e1[0] * p[0] + t.e1[1] * p[1] + t.e1[2] * p[2];
                    if (std::fabs(det) < 1e-20f) continue;
                    const float inv = 1.0f / det;
                    const float s[3] = {o[0] - t.v0[0], o[1] - t.v0[1], o[2] - t.v0[2]};
                    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                    if (u < 0.0f || u > 1.0f) continue;
                    const float q[3] = {s[1] * t.e1[2] - s[2] * t.e1[1], s[2] * t.e1[0] - s[0] * t.e1[2],
                                        s[0] * t.e1[1] - s[1] * t.e1[0]};
                    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
                    if (v < 0.0f || u + v > 1.0f) continue;
                    const float tt = (t.e2[0] * q[0] + t.e2[1] * q[1] + t.e2[2] * q[2]) * inv;
                    if (tt >= 0.0f && tt < best) { best = tt; hit = true; }
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = n.first + 1;
                stack[sp++] = n.first;
            }
        }
    }
    if (hit) tOut = best;
    return hit;
}
