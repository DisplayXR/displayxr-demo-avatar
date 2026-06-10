// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Diffuse irradiance cubemap: cosine-weighted integration of the analytic sky
// over the hemisphere about each output direction. Stores E/π (the main pass
// multiplies by albedo directly).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_common.glsl"
#include "sky.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform P { int face; float roughness; } pc;

void main() {
    vec3 N = dirForFace(pc.face, inUV);
    vec3 up = abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tx = normalize(cross(up, N));
    vec3 ty = cross(N, tx);

    const uint NS = 2048u;
    vec3 irr = vec3(0.0);
    for (uint i = 0u; i < NS; ++i) {
        vec2 Xi = hammersley(i, NS);
        float phi = 2.0 * PI * Xi.x;
        float cosT = sqrt(1.0 - Xi.y);   // cosine-weighted
        float sinT = sqrt(Xi.y);
        vec3 l = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        vec3 wd = tx * l.x + ty * l.y + N * l.z;
        irr += skyRadiance(wd);
    }
    outColor = vec4(irr / float(NS), 1.0);
}
