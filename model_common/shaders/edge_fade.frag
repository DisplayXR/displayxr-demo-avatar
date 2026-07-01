// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Zone edge-fade post-pass (ADR-027 rule 4: blends are CONTENT alpha). Drawn
// as the last fullscreen triangle of the eye render pass with blend factors
// (ZERO, ONE_MINUS_SRC_ALPHA) on color AND alpha, so writing (0,0,0, 1-f)
// multiplies the premultiplied dst by f — fading RGB and A together to fully
// transparent over featherPx pixels at the tile edges. gl_FragCoord-based, so
// the ramp is edge-symmetric regardless of the viewport Y-flip.
#version 450

layout(push_constant) uniform FadePush {
    vec2 tilePx;      // tile (viewport) size in pixels
    float featherPx;  // fade width; caller guarantees > 0
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 px = gl_FragCoord.xy;
    float d = min(min(px.x, pc.tilePx.x - px.x),
                  min(px.y, pc.tilePx.y - px.y));
    float f = clamp(d / pc.featherPx, 0.0, 1.0);
    outColor = vec4(0.0, 0.0, 0.0, 1.0 - f);  // dst *= f
}
