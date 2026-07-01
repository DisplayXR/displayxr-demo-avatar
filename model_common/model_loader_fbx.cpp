// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  FBX loader backend (ufbx) → ModelData.
 *
 * Routed to from model_loader_load() for .fbx. ufbx parses the scene with axis +
 * unit conversion to glTF conventions (right-handed Y-up, metres), so FBX's
 * usual cm / Z-up content lands correctly.
 *
 * Static meshes bake the node's geometry_to_world into the positions (identity
 * model matrix downstream, matching the other backends). **Skinned** meshes keep
 * vertices in geometry space and emit the skin (bone node list + inverse-bind
 * matrices) + the node hierarchy + baked animation clips into the SAME ModelData
 * fields the glTF backend fills, so the format-neutral renderer skins + animates
 * them with no renderer/shader changes:
 *   - per-vertex top-4 joints/weights from the skin deformer;
 *   - ModelSkin.inverseBind = ufbx cluster->geometry_to_bone (geometry→bone);
 *   - ModelNode hierarchy from scene->nodes (local TRS), HELPER_NODES inherit
 *     handling so a plain parent×local walk reproduces ufbx node_to_world;
 *   - animations baked from each anim_stack at a fixed rate (ufbx evaluates the
 *     native FBX curves/layers/pre-post-rotation for us) → Linear keyframes,
 *     constant channels dropped.
 *
 * Materials prefer the PBR maps (base_color / metalness / roughness / emission /
 * normal); when a value is absent we fall back to the legacy FBX (Phong) maps,
 * shimming specular_exponent → roughness via model_loader_material.h. Textures
 * come from the embedded blob when present, else the resolved file path.
 *
 * This TU compiles the vendored ufbx implementation indirectly: ufbx.c is its
 * own C translation unit in the build; here we only use the public API.
 */

#include "third_party/ufbx.h"

#include "model_loader_backends.h"
#include "model_loader_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::string toStr(const ufbx_string& s) {
    return (s.data && s.length) ? std::string(s.data, s.length) : std::string();
}

// ufbx_matrix is a column-major 3×4 affine (cols[3] = translation). Expand to a
// column-major 4×4 (16 floats), the layout ModelSkin::inverseBind expects.
void ufbxMatToCol16(const ufbx_matrix& m, float o[16]) {
    o[0]  = (float)m.cols[0].x; o[1]  = (float)m.cols[0].y; o[2]  = (float)m.cols[0].z; o[3]  = 0.0f;
    o[4]  = (float)m.cols[1].x; o[5]  = (float)m.cols[1].y; o[6]  = (float)m.cols[1].z; o[7]  = 0.0f;
    o[8]  = (float)m.cols[2].x; o[9]  = (float)m.cols[2].y; o[10] = (float)m.cols[2].z; o[11] = 0.0f;
    o[12] = (float)m.cols[3].x; o[13] = (float)m.cols[3].y; o[14] = (float)m.cols[3].z; o[15] = 1.0f;
}

// Decode a ufbx texture → index into out.textures (or -1). Embedded blob first,
// then the resolved absolute path, then the relative path against the model dir,
// then a basename fallback in the model dir. The last step matters for assets
// (e.g. the bundled avatar FBX) whose authored texture paths point at a build
// machine's scratch dir (".../tmp/skins_*.fbm/rgb.jpg") that doesn't exist at
// runtime — we strip to the basename and look beside the model.
int loadUfbxTexture(const ufbx_texture* tex, const std::filesystem::path& dir, ModelData& out) {
    if (!tex) return -1;
    if (tex->content.size > 0)
        return material_load_texture_memory(
            static_cast<const unsigned char*>(tex->content.data), (int)tex->content.size, out);
    const std::string fn = toStr(tex->filename);
    if (!fn.empty()) {
        int idx = material_load_texture_file(fn, out);
        if (idx >= 0) return idx;
    }
    const std::string rel = toStr(tex->relative_filename);
    if (!rel.empty()) {
        int idx = material_load_texture_file((dir / rel).string(), out);
        if (idx >= 0) return idx;
        // Basename fallback: stale/foreign relative path → look beside the model.
        const std::filesystem::path base = std::filesystem::path(rel).filename();
        if (!base.empty())
            return material_load_texture_file((dir / base).string(), out);
    }
    return -1;
}

// Fill a vertex's top-4 joint indices + (renormalised) blend weights for the
// control point `cp`. Joint indices are cluster indices within this deformer,
// matching ModelSkin::joints order (offset by jointBase in the shader).
void fillSkinWeights(const ufbx_skin_deformer* skin, uint32_t cp, ModelVertex& mv) {
    if (cp >= skin->vertices.count) { mv.joints0[0] = 0; mv.weights0[0] = 1.0f; return; }
    const ufbx_skin_vertex sv = skin->vertices.data[cp];
    float    bw[4] = {0, 0, 0, 0};
    uint16_t bj[4] = {0, 0, 0, 0};
    for (uint32_t w = 0; w < sv.num_weights; ++w) {
        const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + w];
        const float wt = (float)sw.weight;
        // Insertion sort into the descending top-4.
        for (int k = 0; k < 4; ++k) {
            if (wt > bw[k]) {
                for (int j = 3; j > k; --j) { bw[j] = bw[j - 1]; bj[j] = bj[j - 1]; }
                bw[k] = wt; bj[k] = (uint16_t)sw.cluster_index;
                break;
            }
        }
    }
    const float sum = bw[0] + bw[1] + bw[2] + bw[3];
    if (sum > 1e-8f) {
        for (int k = 0; k < 4; ++k) { mv.joints0[k] = bj[k]; mv.weights0[k] = bw[k] / sum; }
    } else {
        // Unweighted vertex in a skinned mesh: pin to the first bone so the
        // weighted matrix isn't a zero matrix (which would collapse it to origin).
        mv.joints0[0] = 0; mv.weights0[0] = 1.0f;
    }
}

ModelVertex makeVertex(const ufbx_mesh* mesh, uint32_t ci,
                       const ufbx_matrix& world, const ufbx_matrix& nrmMat,
                       const ufbx_skin_deformer* skin) {
    ModelVertex mv{};
    // Skinned meshes stay in geometry space (the joint matrices carry geom→world
    // via inverseBind); static meshes bake the node world transform.
    ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ci);
    if (!skin) p = ufbx_transform_position(&world, p);
    mv.pos[0] = (float)p.x; mv.pos[1] = (float)p.y; mv.pos[2] = (float)p.z;

    if (mesh->vertex_normal.exists) {
        ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, ci);
        if (!skin) n = ufbx_transform_direction(&nrmMat, n);
        const double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (l > 1e-12) { n.x /= l; n.y /= l; n.z /= l; }
        mv.normal[0] = (float)n.x; mv.normal[1] = (float)n.y; mv.normal[2] = (float)n.z;
    } else {
        mv.normal[1] = 1.0f;
    }
    if (mesh->vertex_uv.exists) {
        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ci);
        mv.uv[0] = (float)uv.x;
        mv.uv[1] = 1.0f - (float)uv.y;  // FBX V is bottom-up → flip to top-down
    }
    if (skin) fillSkinWeights(skin, mesh->vertex_indices.data[ci], mv);
    return mv;
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

// Append a single-property animation channel (Linear-sampled) to a clip.
void addChannel(Animation& anim, int targetNode, AnimPath path,
                const std::vector<float>& times, std::vector<float>&& values) {
    AnimSampler smp;
    smp.input  = times;
    smp.output = std::move(values);
    smp.interp = AnimInterp::Linear;
    AnimChannel ch;
    ch.targetNode = targetNode;
    ch.path       = path;
    ch.sampler    = (int)anim.samplers.size();
    anim.samplers.push_back(std::move(smp));
    anim.channels.push_back(ch);
}

}  // namespace

bool model_load_fbx(const char* path, ModelData& out) {
    if (!path) return false;
    const std::filesystem::path modelDir = std::filesystem::path(path).parent_path();

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;  // match glTF
    opts.target_unit_meters = 1.0f;                  // normalise cm/inch → metres
    opts.generate_missing_normals = true;
    // Insert helper nodes for non-standard FBX scale-inheritance modes so a plain
    // parent×local hierarchy walk reproduces ufbx's node_to_world (the renderer's
    // animation path composes local TRS that way).
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;

    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(path, &opts, &err);
    if (!scene) {
        std::fprintf(stderr, "[model_loader/fbx] '%s': %s\n", path, err.description.data);
        return false;
    }

    // ── Node hierarchy (indexed by typed_id) → ModelData::nodes ───────────────
    // Bind-pose local TRS; the renderer composes world matrices each frame and
    // animation channels (below) override per-frame TRS.
    out.nodes.resize(scene->nodes.count);
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* n = scene->nodes.data[i];
        ModelNode& mn = out.nodes[n->typed_id];
        mn.parent = n->parent ? (int)n->parent->typed_id : -1;
        mn.children.reserve(n->children.count);
        for (size_t c = 0; c < n->children.count; ++c)
            mn.children.push_back((int)n->children.data[c]->typed_id);
        const ufbx_transform& t = n->local_transform;
        mn.translation[0] = (float)t.translation.x; mn.translation[1] = (float)t.translation.y; mn.translation[2] = (float)t.translation.z;
        mn.rotation[0] = (float)t.rotation.x; mn.rotation[1] = (float)t.rotation.y; mn.rotation[2] = (float)t.rotation.z; mn.rotation[3] = (float)t.rotation.w;
        mn.scale[0] = (float)t.scale.x; mn.scale[1] = (float)t.scale.y; mn.scale[2] = (float)t.scale.z;
    }
    if (scene->root_node) out.rootNodes.push_back((int)scene->root_node->typed_id);
    else for (size_t i = 0; i < out.nodes.size(); ++i)
        if (out.nodes[i].parent < 0) out.rootNodes.push_back((int)i);

    // Resolve a ufbx material → ModelData material index, caching the mapping.
    std::unordered_map<const ufbx_material*, int> matIndex;
    int defaultMat = -1;
    auto resolveMaterial = [&](const ufbx_material* m) -> int {
        if (!m) {
            if (defaultMat < 0) {
                ModelMaterial mm{};
                mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
                mm.baseColorFactor[3] = 1.0f;
                mm.metallic = 0.0f; mm.roughness = 0.8f;
                defaultMat = (int)out.materials.size();
                out.materials.push_back(mm);
            }
            return defaultMat;
        }
        auto it = matIndex.find(m);
        if (it != matIndex.end()) return it->second;

        ModelMaterial mm{};
        // Base color: PBR first, else legacy diffuse.
        if (m->pbr.base_color.has_value) {
            mm.baseColorFactor[0] = (float)m->pbr.base_color.value_vec4.x;
            mm.baseColorFactor[1] = (float)m->pbr.base_color.value_vec4.y;
            mm.baseColorFactor[2] = (float)m->pbr.base_color.value_vec4.z;
            mm.baseColorFactor[3] = (float)m->pbr.base_color.value_vec4.w;
        } else if (m->fbx.diffuse_color.has_value) {
            mm.baseColorFactor[0] = (float)m->fbx.diffuse_color.value_vec4.x;
            mm.baseColorFactor[1] = (float)m->fbx.diffuse_color.value_vec4.y;
            mm.baseColorFactor[2] = (float)m->fbx.diffuse_color.value_vec4.z;
            mm.baseColorFactor[3] = 1.0f;
        }
        mm.metallic  = m->pbr.metalness.has_value ? (float)m->pbr.metalness.value_real : 0.0f;
        mm.roughness = m->pbr.roughness.has_value
                         ? (float)m->pbr.roughness.value_real
                         : material_shininess_to_roughness((float)m->fbx.specular_exponent.value_real);
        // Emission: PBR color×factor, else legacy.
        if (m->pbr.emission_color.has_value) {
            const float f = m->pbr.emission_factor.has_value ? (float)m->pbr.emission_factor.value_real : 1.0f;
            mm.emissive[0] = (float)m->pbr.emission_color.value_vec4.x * f;
            mm.emissive[1] = (float)m->pbr.emission_color.value_vec4.y * f;
            mm.emissive[2] = (float)m->pbr.emission_color.value_vec4.z * f;
        } else if (m->fbx.emission_color.has_value) {
            mm.emissive[0] = (float)m->fbx.emission_color.value_vec4.x;
            mm.emissive[1] = (float)m->fbx.emission_color.value_vec4.y;
            mm.emissive[2] = (float)m->fbx.emission_color.value_vec4.z;
        }
        // Textures (no clean combined metallic-roughness in FBX → factors only).
        const ufbx_texture* baseTex = m->pbr.base_color.texture ? m->pbr.base_color.texture
                                                                : m->fbx.diffuse_color.texture;
        const ufbx_texture* nrmTex  = m->pbr.normal_map.texture ? m->pbr.normal_map.texture
                                                                : m->fbx.normal_map.texture;
        mm.baseColorTex = loadUfbxTexture(baseTex, modelDir, out);
        mm.normalTex    = loadUfbxTexture(nrmTex, modelDir, out);
        mm.emissiveTex  = loadUfbxTexture(m->pbr.emission_color.texture, modelDir, out);

        const int idx = (int)out.materials.size();
        out.materials.push_back(mm);
        matIndex.emplace(m, idx);
        return idx;
    };

    // Resolve a skin deformer → {skin index, jointBase}, building ModelSkin once.
    std::unordered_map<const ufbx_skin_deformer*, std::pair<int, uint32_t>> skinCache;
    auto resolveSkin = [&](const ufbx_skin_deformer* d) -> std::pair<int, uint32_t> {
        auto it = skinCache.find(d);
        if (it != skinCache.end()) return it->second;
        ModelSkin sk;
        sk.joints.reserve(d->clusters.count);
        sk.inverseBind.resize(d->clusters.count * 16);
        for (size_t c = 0; c < d->clusters.count; ++c) {
            const ufbx_skin_cluster* cl = d->clusters.data[c];
            sk.joints.push_back(cl->bone_node ? (int)cl->bone_node->typed_id : -1);
            ufbxMatToCol16(cl->geometry_to_bone, &sk.inverseBind[c * 16]);
        }
        const std::pair<int, uint32_t> pr((int)out.skins.size(), out.totalJoints);
        out.totalJoints += (uint32_t)d->clusters.count;
        out.skins.push_back(std::move(sk));
        skinCache.emplace(d, pr);
        return pr;
    };

    // Walk mesh instances; bake node world transform (static) or keep geometry
    // space (skinned); split by material part.
    std::vector<uint32_t> tri;
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];
        tri.resize(mesh->max_face_triangles * 3);

        // First skin deformer with clusters drives this mesh (one per mesh is
        // the norm); null → static.
        const ufbx_skin_deformer* skin = nullptr;
        for (size_t si = 0; si < mesh->skin_deformers.count; ++si) {
            if (mesh->skin_deformers.data[si]->clusters.count > 0) { skin = mesh->skin_deformers.data[si]; break; }
        }
        int skinIndex = -1; uint32_t jointBase = 0;
        if (skin) { auto pr = resolveSkin(skin); skinIndex = pr.first; jointBase = pr.second; }

        for (size_t ni = 0; ni < mesh->instances.count; ++ni) {
            const ufbx_node* node = mesh->instances.data[ni];
            const ufbx_matrix world  = node->geometry_to_world;
            const ufbx_matrix nrmMat = ufbx_matrix_for_normals(&world);

            for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
                const ufbx_mesh_part& part = mesh->material_parts.data[pi];
                if (part.num_triangles == 0) continue;

                const ufbx_material* mat =
                    (part.index < node->materials.count) ? node->materials.data[part.index] : nullptr;

                ModelPrimitive mp{};
                mp.firstVertex = (uint32_t)out.vertices.size();
                mp.firstIndex  = (uint32_t)out.indices.size();
                mp.material = resolveMaterial(mat);
                mp.node = -1; mp.morph = -1;
                mp.skin = skinIndex;          // -1 when static
                mp.jointBase = (int)jointBase;
                std::memset(mp.modelMatrix, 0, sizeof(mp.modelMatrix));
                mp.modelMatrix[0] = mp.modelMatrix[5] = mp.modelMatrix[10] = mp.modelMatrix[15] = 1.0f;

                for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                    const ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                    const uint32_t nTri = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                    for (uint32_t t = 0; t < nTri * 3; ++t) {
                        const ModelVertex v = makeVertex(mesh, tri[t], world, nrmMat, skin);
                        const uint32_t gi = (uint32_t)out.vertices.size();
                        out.vertices.push_back(v);
                        out.indices.push_back(gi);
                        accumulateBBox(out, v.pos);
                    }
                }
                mp.vertexCount = (uint32_t)out.vertices.size() - mp.firstVertex;
                mp.indexCount  = (uint32_t)out.indices.size()  - mp.firstIndex;
                if (mp.indexCount > 0) out.primitives.push_back(mp);
            }
        }
    }

    // ── Animation: bake each stack to Linear keyframes ────────────────────────
    // ufbx evaluates the native FBX curves/layers/pre-post-rotation per node, so
    // we sample local transforms at a fixed rate and store them as Linear keys
    // (the renderer slerps rotations). Constant channels are dropped.
    constexpr double kFps = 30.0;
    constexpr float  kEps = 1e-6f;
    for (size_t s = 0; s < scene->anim_stacks.count; ++s) {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[s];
        const double dur = stack->time_end - stack->time_begin;
        if (dur < 1e-4) continue;
        const int nS = std::max(2, (int)std::ceil(dur * kFps) + 1);

        Animation anim;
        anim.name = toStr(stack->name);
        anim.duration = (float)dur;

        std::vector<float> times((size_t)nS);
        for (int k = 0; k < nS; ++k) times[(size_t)k] = (float)(dur * k / (nS - 1));

        for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
            const ufbx_node* node = scene->nodes.data[ni];
            std::vector<float> T((size_t)nS * 3), R((size_t)nS * 4), S((size_t)nS * 3);
            bool varT = false, varR = false, varS = false;
            for (int k = 0; k < nS; ++k) {
                const double at = stack->time_begin + dur * k / (nS - 1);
                const ufbx_transform tr = ufbx_evaluate_transform(stack->anim, node, at);
                float* t = &T[(size_t)k * 3]; float* r = &R[(size_t)k * 4]; float* sc = &S[(size_t)k * 3];
                t[0] = (float)tr.translation.x; t[1] = (float)tr.translation.y; t[2] = (float)tr.translation.z;
                r[0] = (float)tr.rotation.x; r[1] = (float)tr.rotation.y; r[2] = (float)tr.rotation.z; r[3] = (float)tr.rotation.w;
                sc[0] = (float)tr.scale.x; sc[1] = (float)tr.scale.y; sc[2] = (float)tr.scale.z;
                if (k > 0) {
                    for (int c = 0; c < 3; ++c) if (std::fabs(T[(size_t)k * 3 + c] - T[c]) > kEps) varT = true;
                    for (int c = 0; c < 4; ++c) if (std::fabs(R[(size_t)k * 4 + c] - R[c]) > kEps) varR = true;
                    for (int c = 0; c < 3; ++c) if (std::fabs(S[(size_t)k * 3 + c] - S[c]) > kEps) varS = true;
                }
            }
            const int nodeIdx = (int)node->typed_id;
            if (varT) addChannel(anim, nodeIdx, AnimPath::Translation, times, std::move(T));
            if (varR) addChannel(anim, nodeIdx, AnimPath::Rotation,    times, std::move(R));
            if (varS) addChannel(anim, nodeIdx, AnimPath::Scale,       times, std::move(S));
        }
        if (!anim.channels.empty()) out.animations.push_back(std::move(anim));
    }

    const size_t meshCount = scene->meshes.count;
    ufbx_free_scene(scene);

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader/fbx] '%s': no drawable triangle geometry\n", path);
        return false;
    }
    std::fprintf(stderr,
        "[model_loader/fbx] '%s': %zu meshes, %u prims, %zu verts, %zu materials, %zu textures, "
        "%zu skins (%u joints), %zu anims\n",
        path, meshCount, out.primitiveCount, out.vertices.size(),
        out.materials.size(), out.textures.size(),
        out.skins.size(), out.totalJoints, out.animations.size());
    return true;
}
