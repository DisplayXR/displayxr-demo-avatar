// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Fullscreen-triangle vertex shader for the IBL generation passes. Emits a
// single oversized triangle covering the viewport; outUV spans [-1,1] across
// the visible NDC region (used by the cube-face passes to reconstruct a
// per-texel direction).
#version 450

layout(location = 0) out vec2 outUV;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0),(2,0),(0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    outUV = p * 2.0 - 1.0;   // [-1,1]
}
