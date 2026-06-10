// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Split-sum BRDF integration LUT (Karis). x = NdotV, y = roughness → (scale, bias).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ibl_common.glsl"

layout(location = 0) in vec2 inUV;   // [-1,1]
layout(location = 0) out vec4 outColor;

float gSchlick(float ndot, float k) { return ndot / (ndot * (1.0 - k) + k); }
float gSmithIBL(float NdotV, float NdotL, float rough) {
    float k = (rough * rough) / 2.0;
    return gSchlick(NdotV, k) * gSchlick(NdotL, k);
}

void main() {
    vec2 uv = inUV * 0.5 + 0.5;           // → [0,1]
    float NdotV = max(uv.x, 1e-3);
    float rough = uv.y;
    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);
    float A = 0.0, B = 0.0;
    const uint NS = 512u;
    for (uint i = 0u; i < NS; ++i) {
        vec2 Xi = hammersley(i, NS);
        vec3 H = importanceSampleGGX(Xi, rough, N);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0), NdotH = max(H.z, 0.0), VdotH = max(dot(V, H), 0.0);
        if (NdotL > 0.0) {
            float G = gSmithIBL(NdotV, NdotL, rough);
            float Gvis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * Gvis;
            B += Fc * Gvis;
        }
    }
    outColor = vec4(A / float(NS), B / float(NS), 0.0, 1.0);
}
