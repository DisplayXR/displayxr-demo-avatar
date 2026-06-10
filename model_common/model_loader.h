// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU-side glTF 2.0 loader (tinygltf) → flat GPU-upload-friendly form.
 *
 * Vendor-neutral analog of 3dgs_common/gs_scene_loader.h. Walks the default
 * scene's node hierarchy, bakes each node's world transform, flattens every
 * mesh primitive into one interleaved vertex buffer + index buffer, and
 * decodes material textures (base-color, metallic-roughness, normal,
 * occlusion, emissive) to RGBA8.
 *
 * Scope: position + normal + uv0 geometry, plus node/TRS animation (Phase 1 —
 * the node graph + animations[] are retained for per-frame world-matrix
 * recompute). Skinning/morph targets are follow-ups. tinygltf's bundled stb is
 * compiled file-local
 * (STB_IMAGE_STATIC) so it doesn't clash with common/'s stb implementation.
 * See ../PORTING.md.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ModelVertex {
    float    pos[3];
    float    normal[3];
    float    uv[2];
    // Skinning (Phase 2). All-zero weights0 ⇒ vertex is not skinned; the
    // renderer flags non-skinned primitives so the shader keeps the static
    // push-constant model path. JOINTS_0 (u8/u16) is widened to u16; WEIGHTS_0
    // (float or normalized int) is decoded to float at load.
    uint16_t joints0[4]  = {0, 0, 0, 0};
    float    weights0[4] = {0, 0, 0, 0};
};

// Decoded RGBA8 texture image. Indices below reference ModelData::textures.
struct ModelTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
};

struct ModelMaterial {
    float baseColorFactor[4] = {1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3] = {0, 0, 0};
    // Texture indices into ModelData::textures, or -1 when absent (the
    // renderer then binds a glTF-correct default: white, or flat normal).
    int baseColorTex = -1;
    int metallicRoughnessTex = -1;
    int normalTex = -1;
    int occlusionTex = -1;
    int emissiveTex = -1;
};

struct ModelPrimitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int      material = -1;        // index into ModelData::materials, or -1
    int      node = -1;            // owning node (index into ModelData::nodes);
                                   // used to re-fetch the animated world matrix
    float    modelMatrix[16];      // baked node world transform (column-major).
                                   // Static fast-path value; overwritten per
                                   // frame when an animation drives this node.
    int      skin = -1;            // glTF skin index, or -1 (not skinned). When
                                   // ≥0 the renderer uses an identity model
                                   // matrix and the joint-matrix SSBO instead.
    int      jointBase = 0;        // offset of this skin's joints in the flat
                                   // joint-matrix array (ModelData::totalJoints).
    uint32_t firstVertex = 0;      // base vertex index (a primitive's verts are
    uint32_t vertexCount = 0;      // contiguous) — the range the morph blend writes.
    int      morph = -1;           // index into ModelData::morphs, or -1 (no targets).
};

// ── Animation (Phase 1: node TRS only; no skinning/morph) ────────────────────
// The node hierarchy is retained so world matrices can be recomputed per frame.
// Header stays glm-free (included by platform code); transforms are plain float
// arrays, consistent with ModelPrimitive::modelMatrix.

struct ModelNode {
    int parent = -1;               // -1 = root
    std::vector<int> children;     // indices into ModelData::nodes
    int mesh = -1;                 // index into the source mesh list, or -1
    // Base local TRS (the bind-pose values from the glTF node). Animation
    // channels override these per frame; untargeted components keep the base.
    float translation[3] = {0, 0, 0};
    float rotation[4]    = {0, 0, 0, 1};   // quaternion (x, y, z, w)
    float scale[3]       = {1, 1, 1};
    bool  hasMatrix = false;       // node specified an explicit local matrix
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // used if hasMatrix
    // Live morph weights (Phase 3). Size = the node's mesh's morph-target count;
    // seeded from node/mesh defaults, overwritten per frame by a Weights channel.
    std::vector<float> weights;
};

enum class AnimInterp { Linear, Step, CubicSpline };
enum class AnimPath   { Translation, Rotation, Scale, Weights };  // Weights = morph (Phase 3)

struct AnimSampler {
    std::vector<float> input;      // keyframe times (seconds), ascending
    std::vector<float> output;     // flattened values; element stride from path.
                                   // CUBICSPLINE packs 3 elems/key: in,val,out.
    AnimInterp interp = AnimInterp::Linear;
};

struct AnimChannel {
    int       targetNode = -1;     // index into ModelData::nodes
    AnimPath  path = AnimPath::Translation;
    int       sampler = -1;        // index into Animation::samplers
};

struct Animation {
    std::string name;
    std::vector<AnimSampler> samplers;
    std::vector<AnimChannel> channels;
    float duration = 0.0f;         // max last-input time across samplers (seconds)
};

// ── Skinning (Phase 2) ───────────────────────────────────────────────────────
// Per frame the renderer computes jointMatrix[i] = nodeWorld[joints[i]] *
// inverseBind[i] (the skinned mesh node's own transform is intentionally NOT
// applied — glTF ignores it for skinned meshes; vertices stay in skin space and
// the draw uses an identity model matrix).
struct ModelSkin {
    std::vector<int>   joints;       // node indices (this skin's joint list)
    std::vector<float> inverseBind;  // 16 floats/joint (MAT4, column-major);
                                     // identity per joint if the accessor is absent
};

// ── Morph targets (Phase 3) ──────────────────────────────────────────────────
// Per-vertex position/normal deltas for each target of one primitive. The
// renderer blends morphed = base + Σ weightᵢ·deltaᵢ on the CPU each frame.
// Deltas are flat: target t, vertex v, component c → [(t*vertexCount + v)*3 + c].
struct ModelMorph {
    uint32_t targetCount = 0;
    uint32_t vertexCount = 0;
    std::vector<float> posDeltas;    // targetCount * vertexCount * 3
    std::vector<float> nrmDeltas;    // same layout; empty when no NORMAL deltas
};

struct ModelData {
    std::vector<ModelVertex>    vertices;
    std::vector<uint32_t>       indices;
    std::vector<ModelTexture>   textures;
    std::vector<ModelMaterial>  materials;
    std::vector<ModelPrimitive> primitives;

    // Node graph + clips (retained for per-frame animation). Empty when the
    // model has no animations → renderer keeps the once-baked static matrices.
    std::vector<ModelNode>      nodes;
    std::vector<Animation>      animations;
    std::vector<int>            rootNodes;   // scene roots (indices into nodes)

    // Skins (Phase 2). Empty when the model has no skinned meshes. Joint
    // matrices for every skin are packed back-to-back; a primitive's jointBase
    // is its skin's offset into that flat array.
    std::vector<ModelSkin>      skins;
    uint32_t totalJoints = 0;                // sum of joint counts across skins

    // Morph targets (Phase 3). Empty when the model has none. A primitive's
    // ModelPrimitive::morph indexes into this; its owning node holds the weights.
    std::vector<ModelMorph>     morphs;

    uint32_t primitiveCount = 0;
    // World-space AABB over all primitives (bind-pose node transforms applied).
    float bboxMin[3] = {0, 0, 0};
    float bboxMax[3] = {0, 0, 0};
    bool  hasBBox = false;
};

// Parse a glTF 2.0 file (.glb or .gltf). Returns false on parse failure or if
// no drawable geometry was found.
bool model_loader_load(const char* gltfPath, ModelData& out);

// ── Path helpers (replace the GS scene-loader's .ply/.spz equivalents) ────
bool model_validate_file(const std::string& path);
std::string model_basename(const std::string& path);
std::string model_filesize_str(const std::string& path);
