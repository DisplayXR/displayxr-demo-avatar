// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Procedural analytic sky used as the IBL environment source (irradiance +
// prefilter generation). Sun direction matches the main pass's directional
// light so the lit + reflected highlights agree. Returns linear radiance.

vec3 skyRadiance(vec3 dir) {
    vec3 sun = normalize(vec3(0.4, 0.8, 0.5));
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.70, 0.74, 0.82);
    vec3 zenith  = vec3(0.22, 0.38, 0.72);
    vec3 ground  = vec3(0.16, 0.15, 0.14);
    vec3 col = (dir.y >= 0.0)
        ? mix(horizon, zenith, pow(up, 0.6))
        : mix(horizon, ground, clamp(-dir.y * 2.0, 0.0, 1.0));
    float s = max(dot(normalize(dir), sun), 0.0);
    col += vec3(1.0, 0.96, 0.88) * pow(s, 350.0) * 4.0;   // sun disk
    col += vec3(1.0, 0.92, 0.78) * pow(s, 4.0)   * 0.25;  // sun glow
    return col;
}
