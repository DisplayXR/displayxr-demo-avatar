// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Background skybox. Reconstructs a world-space view ray per pixel from
// inverse(viewProj) and samples the PREFILTERED environment cube at a high mip
// (blurred) rather than the sharp analytic sky: the background sits far from
// the display's zero-disparity plane, where sharp high-contrast features (the
// sun disk, hard gradients) cause lightfield cross-talk / ghosting. A soft sky
// stays comfortable. Drawn (opaque mode only) behind the model, depth off.
#version 450

layout(location = 0) in vec2 inUV;   // NDC [-1,1]

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 view;
    vec4 cameraPos;
    vec4 lightDir;
    mat4 invViewProj;
} ubo;
layout(set = 2, binding = 1) uniform samplerCube prefilteredMap;

layout(location = 0) out vec4 outColor;

// Inverse sRGB EOTF (accurate piecewise). Must match pbr.frag so the sky and
// the model encode identically. Gated by ubo.cameraPos.w (1 = encode, 0 = skip).
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThan(c, vec3(0.0031308))));
}

void main() {
    vec4 farp = ubo.invViewProj * vec4(inUV, 1.0, 1.0);
    vec3 world = farp.xyz / farp.w;
    vec3 dir = normalize(world - ubo.cameraPos.xyz);
    // Blur the background: sample well up the roughness-mip chain.
    float lod = float(textureQueryLevels(prefilteredMap) - 1) * 0.6;
    vec3 color = textureLod(prefilteredMap, dir, lod).rgb;
    if (ubo.cameraPos.w > 0.5) color = linearToSrgb(color);
    outColor = vec4(color, 1.0);
}
