// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Prefiltered specular environment: GGX importance-sample the analytic sky for
// the mip's roughness. One mip per roughness level (set via push constant).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_common.glsl"
#include "sky.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform P { int face; float roughness; } pc;

void main() {
    vec3 N = dirForFace(pc.face, inUV);
    vec3 V = N;   // common approximation: view = reflection = normal

    const uint NS = 1024u;
    vec3 color = vec3(0.0);
    float totalW = 0.0;
    for (uint i = 0u; i < NS; ++i) {
        vec2 Xi = hammersley(i, NS);
        vec3 H = importanceSampleGGX(Xi, pc.roughness, N);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            color += skyRadiance(L) * NdotL;
            totalW += NdotL;
        }
    }
    outColor = vec4(color / max(totalW, 1e-3), 1.0);
}
