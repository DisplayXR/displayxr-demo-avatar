// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Metallic-roughness PBR fragment shader (Cook-Torrance GGX) with the full
// glTF material texture set (base-color, metallic-roughness, normal, occlusion,
// emissive), sRGB-correct sampling, tangent-free normal mapping (Schüler's
// cotangent frame from screen-space derivatives), one directional light, and a
// hemispherical-ambient stand-in for IBL (so metal reflects sky/ground instead
// of going black). Real image-based lighting (env cubemap + prefiltered specular
// + BRDF LUT) is the remaining follow-up. See ../../PORTING.md.
#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inViewZ;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 view;
    vec4 cameraPos;
    vec4 lightDir;     // .xyz = light dir, .w = clipFar (view-space; 0=off)
    mat4 invViewProj;  // (skybox only)
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;  // linear (glTF factor)
    vec4 mrParams;         // x=metallic factor, y=roughness factor
    vec4 emissive;         // rgb emissive factor (linear)
} pc;

// Set 1: per-material textures. Absent maps default to white / flat normal.
layout(set = 1, binding = 0) uniform sampler2D baseColorTex;
layout(set = 1, binding = 1) uniform sampler2D mrTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;
layout(set = 1, binding = 4) uniform sampler2D emissiveTex;

// Set 2: image-based lighting (generated from the analytic sky).
layout(set = 2, binding = 0) uniform samplerCube irradianceMap;   // diffuse
layout(set = 2, binding = 1) uniform samplerCube prefilteredMap;  // specular (roughness mips)
layout(set = 2, binding = 2) uniform sampler2D   brdfLUT;         // split-sum scale/bias

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float D_GGX(float ndoth, float a) {
    float a2 = a * a;
    float d = (ndoth * ndoth) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
float G_SchlickSmith(float ndotv, float ndotl, float a) {
    float k = (a * a) * 0.5;
    float gv = ndotv / (ndotv * (1.0 - k) + k);
    float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}
vec3 F_Schlick(float cosT, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}
vec3 srgbToLinear(vec3 c) { return pow(c, vec3(2.2)); }
// Inverse sRGB EOTF (accurate piecewise), for encoding the final linear color
// into a UNORM swapchain. Gated by ubo.cameraPos.w (1 = encode, 0 = skip).
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThan(c, vec3(0.0031308))));
}
// Fresnel with a roughness-aware ceiling (for ambient specular).
vec3 F_SchlickRoughness(float cosT, vec3 f0, float rough) {
    return f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// Tangent-free normal mapping (Christian Schüler). Builds a cotangent frame
// from screen-space derivatives of position + uv, so no TANGENT attribute is
// needed. N is the (viewer-facing) geometric normal.
vec3 perturbNormal(vec3 N, vec2 uv) {
    vec3 mapN = texture(normalTex, uv).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(inWorldPos), dp2 = dFdy(inWorldPos);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // No usable UV gradient (mesh without TEXCOORD_0, e.g. AnimatedMorphCube) →
    // T/B collapse to 0 and inversesqrt(0) would poison N with NaNs. Keep the
    // geometric normal in that case (a flat normal map is identity here anyway).
    float maxlen = max(dot(T, T), dot(B, B));
    if (maxlen < 1e-12) return N;
    float invmax = inversesqrt(maxlen);
    return normalize(mat3(T * invmax, B * invmax, N) * mapN);
}

void main() {
    // Foreground-only clip (transparent mode): drop geometry behind the
    // display plane. 0 = disabled (opaque path unaffected).
    if (ubo.lightDir.w > 0.0 && inViewZ > ubo.lightDir.w) discard;

    vec4 baseSample = texture(baseColorTex, inUV);
    vec3 albedo = srgbToLinear(baseSample.rgb) * pc.baseColorFactor.rgb;

    vec3 mr = texture(mrTex, inUV).rgb;        // g=roughness, b=metallic (linear)
    float metallic  = clamp(mr.b * pc.mrParams.x, 0.0, 1.0);
    float roughness = clamp(mr.g * pc.mrParams.y, 0.04, 1.0);
    float a = roughness * roughness;
    float ao = texture(occlusionTex, inUV).r;
    vec3 emissive = srgbToLinear(texture(emissiveTex, inUV).rgb) * pc.emissive.rgb;

    vec3 V = normalize(ubo.cameraPos.xyz - inWorldPos);
    vec3 Ng = normalize(inNormal);
    // Two-sided: flip the normal for genuinely back-facing triangles (cull is
    // NONE) using the rasterizer's winding, NOT dot(N,V). The view test wrongly
    // flips large flat *front* faces seen near edge-on, sending their normal to
    // the dark lower hemisphere of the IBL irradiance cube (the dark-torso
    // artifact on low-poly skinned meshes like Fox). gl_FrontFacing stays
    // geometric under the renderer's Y-flipped projection, so flip only true
    // back-faces — visible front faces keep their authored outward normal.
    if (!gl_FrontFacing) Ng = -Ng;
    vec3 N = perturbNormal(Ng, inUV);

    vec3 L = normalize(ubo.lightDir.xyz);
    vec3 H = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, H), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    // Direct directional light.
    float D = D_GGX(ndoth, a);
    float G = G_SchlickSmith(ndotv, ndotl, a);
    vec3  F = F_Schlick(max(dot(H, V), 0.0), f0);
    vec3 spec = (D * G) * F / max(4.0 * ndotv * ndotl, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    vec3 direct = (kd * albedo / PI + spec) * vec3(3.0) * ndotl;

    // Ambient = image-based lighting (split-sum): irradiance cube for diffuse,
    // prefiltered cube + BRDF LUT for specular.
    vec3 Fr = F_SchlickRoughness(ndotv, f0, roughness);
    vec3 diffuseIBL = texture(irradianceMap, N).rgb * albedo * (1.0 - metallic);
    float maxLod = float(textureQueryLevels(prefilteredMap) - 1);
    vec3 prefiltered = textureLod(prefilteredMap, reflect(-V, N), roughness * maxLod).rgb;
    vec2 ab = texture(brdfLUT, vec2(ndotv, roughness)).rg;
    vec3 specularIBL = prefiltered * (Fr * ab.x + ab.y);
    vec3 ambient = (diffuseIBL + specularIBL) * ao;

    vec3 color = direct + ambient + emissive;
    if (ubo.cameraPos.w > 0.5) color = linearToSrgb(color);
    outColor = vec4(color, baseSample.a * pc.baseColorFactor.a);
}
