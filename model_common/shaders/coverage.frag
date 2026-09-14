#version 450
// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unclipped-silhouette coverage (displayxr-runtime#1470; modelviewer PR #128).
//
// Paired with pbr.vert, into a single R8 attachment, to produce the silhouette
// the avatar WOULD have at an unrestricted rear depth budget. It is
// deliberately the emptiest possible fragment shader: the one and only thing
// that must NOT happen here is pbr.frag's
//
//     if (ubo.lightDir.w > 0.0 && inViewZ > ubo.lightDir.w) discard;
//
// because that discard is exactly what makes the rendered alpha a function of
// the budget the runtime published — the feedback loop that makes the rear clip
// oscillate. The click-through window region keeps the clipped alpha; it is a
// visual clip and must stay one.
//
// It declares no inputs at all. pbr.vert's outputs (worldPos/normal/uv/viewZ)
// are simply left unconsumed, which Vulkan's shader-interface matching allows
// (extra vertex outputs are fine; a fragment input with no producer is not).
// Nothing here samples a texture, so no material descriptor set (set 1) needs
// to be bound for this pipeline.
//
// vec4 rather than float: the attachment is R8_UNORM and the pipeline's write
// mask is R only, so g/b/a are dropped — but a vec4 output against a
// single-component attachment is the form every driver in the matrix
// (native VK + MoltenVK) handles without comment.
layout(location = 0) out vec4 outCoverage;

void main() {
    outCoverage = vec4(1.0, 0.0, 0.0, 1.0);
}
