// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Metallic-roughness PBR vertex shader. Static geometry uses the per-primitive
// push-constant model matrix; skinned primitives (mrParams.z > 0.5) use linear-
// blend skinning from the set-3 joint-matrix SSBO instead, with an identity push
// model. viewProj is proj * Y-flipped-view (see ModelRenderer::updateUniforms),
// so the model orientation matches the GS demo's pose convention exactly.
// Morph targets are a follow-up. See ../../PORTING.md.
#version 450

layout(location = 0) in vec3  inPos;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uvec4 inJoints0;   // skin joint indices (u16×4)
layout(location = 4) in vec4  inWeights0;  // skin blend weights

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;     // proj * Y-flipped view
    mat4 view;         // Z-forward-adjusted view (for the foreground clip)
    vec4 cameraPos;    // world-space, .w unused
    vec4 lightDir;     // .xyz = world dir TO light, .w = clipFar (view-space; 0=off)
    mat4 invViewProj;  // (skybox only)
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 mrParams;     // x=metallic, y=roughness, z=isSkinned(0/1), w=jointBase
    vec4 emissive;     // rgb
} pc;

// Joint matrices for all skins, packed back-to-back; jointBase (mrParams.w)
// offsets into this for the current primitive's skin.
layout(std430, set = 3, binding = 0) readonly buffer Joints {
    mat4 jointMat[];
};

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out float outViewZ;   // view-space forward distance

void main() {
    mat4 m = pc.model;
    if (pc.mrParams.z > 0.5) {
        // Linear-blend skinning. Joint matrices already carry skin→world, so the
        // push model is identity and this fully places the vertex.
        int base = int(pc.mrParams.w);
        m = inWeights0.x * jointMat[base + int(inJoints0.x)]
          + inWeights0.y * jointMat[base + int(inJoints0.y)]
          + inWeights0.z * jointMat[base + int(inJoints0.z)]
          + inWeights0.w * jointMat[base + int(inJoints0.w)];
    }
    vec4 world = m * vec4(inPos, 1.0);
    outWorldPos = world.xyz;
    outNormal = normalize(mat3(m) * inNormal);
    outUV = inUV;
    outViewZ = (ubo.view * world).z;
    gl_Position = ubo.viewProj * world;
}
