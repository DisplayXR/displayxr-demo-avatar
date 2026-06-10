// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Shared helpers for the IBL generation passes: Hammersley sampling, GGX
// importance sampling, and the cube-face direction reconstruction used to turn
// a fullscreen-triangle texel (uv in [-1,1] + face index) into a world dir.

const float PI = 3.14159265359;

float radicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), radicalInverse_VdC(i));
}
vec3 importanceSampleGGX(vec2 Xi, float roughness, vec3 N) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosT = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 up = abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tx = normalize(cross(up, N));
    vec3 ty = cross(N, tx);
    return normalize(tx * H.x + ty * H.y + N * H.z);
}

// Cube-face direction from a fullscreen texel. `uv` in [-1,1]; face order is
// Vulkan's +X,-X,+Y,-Y,+Z,-Z (layers 0..5).
vec3 dirForFace(int face, vec2 uv) {
    vec3 d;
    if      (face == 0) d = vec3(  1.0, -uv.y, -uv.x);
    else if (face == 1) d = vec3( -1.0, -uv.y,  uv.x);
    else if (face == 2) d = vec3( uv.x,   1.0,  uv.y);
    else if (face == 3) d = vec3( uv.x,  -1.0, -uv.y);
    else if (face == 4) d = vec3( uv.x, -uv.y,   1.0);
    else                d = vec3(-uv.x, -uv.y,  -1.0);
    return normalize(d);
}
