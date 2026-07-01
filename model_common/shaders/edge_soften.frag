// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Alpha edge-softening post-pass (Step B of PM0034 anti-aliasing).
// Reads the MSAA-resolved color image and applies a 3×3 alpha-weighted Gaussian
// to widen the silhouette transition band from ~1 pixel (MSAA only) to ~3 pixels.
//
// Alpha: standard 1-2-1 separable Gaussian (sum = 1). The broadened ramp
// eliminates the visible quantisation steps produced by finite MSAA sample
// count, which is especially pronounced on the lenticular 3D display where
// individual pixels are optically magnified per viewing zone.
//
// RGB: alpha-weighted blend (each neighbour weighted by its alpha × Gaussian
// weight). This propagates the tiger body colour outward into the newly
// semi-transparent fringe pixels instead of pulling in the undefined zero-RGB
// values from fully-transparent background pixels, preventing black fringing
// under straight-alpha (non-premultiplied) Windows DWM / DisplayXR composition.
//
// Interior-opaque (all neighbours α≈1) and exterior-transparent (all α≈0)
// pixels are mathematically passed through unchanged: the Gaussian of
// identical values is the identical value, and α=0 RGB is invisible regardless.
#version 450

layout(location = 0) in vec2 inUV;  // from fullscreen.vert [-1,1]; unused — positions from gl_FragCoord
layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 imgSize = vec2(textureSize(srcTex, 0));
    vec2 d       = 1.0 / imgSize;
    vec2 uv0     = gl_FragCoord.xy / imgSize;   // centre of this texel

    // 3×3 separable Gaussian (1-2-1 kernel, sum = 16):
    //   gw(dx,dy) = gx * gy / 16,  gx = gy = 2 at offset 0, else 1
    float alphaAcc = 0.0;
    vec3  rgbAcc   = vec3(0.0);
    float rgbW     = 0.0;

    for (int dy = -1; dy <= 1; ++dy) {
        float gy = (dy == 0) ? 2.0 : 1.0;
        for (int dx = -1; dx <= 1; ++dx) {
            float gx = (dx == 0) ? 2.0 : 1.0;
            float gw = gx * gy * (1.0 / 16.0);
            vec4  s  = texture(srcTex, uv0 + vec2(float(dx), float(dy)) * d);
            alphaAcc += s.a * gw;
            float aw  = s.a * gw;
            rgbAcc   += s.rgb * aw;
            rgbW     += aw;
        }
    }

    // Alpha-weighted RGB: if no opaque neighbours at all (exterior transparent
    // region), fall back to the centre pixel RGB (invisible at alpha≈0 anyway).
    vec3 newRGB = (rgbW > 1e-5) ? rgbAcc / rgbW : texture(srcTex, uv0).rgb;
    outColor    = vec4(newRGB, alphaAcc);
}
