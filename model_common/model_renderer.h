// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  glTF 2.0 PBR model renderer for the DisplayXR model-viewer demo.
 *
 * Vendor-neutral analog of 3dgs_common/gs_renderer.h. Loads a .glb/.gltf model
 * via model_loader (tinygltf) and rasterises it with a metallic-roughness PBR
 * pass into an internal colour image, then blits that into the per-eye
 * swapchain viewport region — reusing the exact viewport-copy + transparency
 * scaffolding the GS renderer uses, so it drops into the platform code with a
 * mechanical rename.
 *
 * v1 scope: static geometry, material FACTORS (base color, metallic,
 * roughness, emissive), one directional light + flat ambient. Textures, IBL,
 * skinning and animation are follow-ups; the shader/CMake hooks are in place.
 * See ../PORTING.md.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>
#include <vector>
#include "model_vulkan_utils.h"
#include "model_loader.h"

struct ModelRenderer {
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    bool loadModel(const char* gltfPath);
    bool loadDebugModel();
    bool hasModel() const;
    const std::string& modelPath() const;
    uint32_t primitiveCount() const;

    // Advance the active animation clip by dtSeconds and refresh per-primitive
    // model matrices. No-op (static fast-path) when the model has no animation.
    // Call once per frame, before renderEye. Frozen while paused (the pose is
    // still recomputed, so a clip switch / pause shows the correct frame).
    void updateAnimation(float dtSeconds);

    // ── Playback control (Phase 4). All no-op without animations. ────────────
    void setActiveAnimation(int index);   // clamps/wraps; resets time + bind pose
    void cycleAnimation();                 // → next clip (wraps); no-op if <2 clips
    void togglePaused();
    bool isPaused() const { return paused_; }
    bool hasAnimations() const { return !animations_.empty(); }
    // Fills the active clip's status for the HUD; false when the model has no
    // clips. name = clip name, or "Clip <i>" when the glTF clip is unnamed.
    bool getPlaybackInfo(std::string& name, int& index, int& count,
                         float& time, float& duration, bool& playing) const;
    // ── Agent-facing read/seek accessors (XR_EXT_mcp_tools adoption). ───────
    int  animationCount() const { return (int)animations_.size(); }
    int  activeAnimation() const { return activeAnim_; }
    void setPaused(bool p) { paused_ = p; }
    // Clip name + duration by index; the name falls back to "Clip <i>" exactly
    // like getPlaybackInfo so list_animations and the HUD agree.
    bool getAnimationInfo(int index, std::string& name, float& duration) const {
        if (index < 0 || index >= (int)animations_.size()) return false;
        name = animations_[index].name.empty()
            ? ("Clip " + std::to_string(index)) : animations_[index].name;
        duration = animations_[index].duration;
        return true;
    }

    bool getSceneBBox(float outMin[3], float outMax[3]) const;
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;
    // Smoothed world-space centroid of the active skeleton (mean joint
    // position), updated by updateAnimation. Lets the platform bind the
    // display rig to a moving/skinned subject so it stays centered + at the
    // ZDP. Returns false for static / non-skinned models (no binding).
    bool getAnimatedAnchor(float out[3]) const;
    bool pickSurface(const float rayOrigin[3], const float rayDir[3],
                     float hitPos[3], float maxDistance = 100.0f) const;
    float findBestYaw(const float displayCenter[3],
                      const float viewerOffsetLocal[3],
                      uint32_t numCandidates = 8) const;

    void renderEye(VkImage swapchainImage,
                   VkFormat swapchainFormat,
                   uint32_t imageWidth,
                   uint32_t imageHeight,
                   uint32_t viewportX,
                   uint32_t viewportY,
                   uint32_t viewportWidth,
                   uint32_t viewportHeight,
                   const float viewMatrix[16],
                   const float projMatrix[16],
                   bool transparentBg = false,
                   float clipFarViewSpace = 0.0f);

    void cleanup();
    ~ModelRenderer();

private:
    // Push-constant block (must match shaders/pbr.{vert,frag}). 112 bytes.
    struct PushBlock {
        float model[16];
        float baseColorFactor[4];
        float mrParams[4];   // x=metallic, y=roughness, z=isSkinned(0/1), w=jointBase
        float emissive[4];   // rgb
    };
    // Set-0 uniform buffer (must match shaders/pbr.{vert,frag}).
    struct UniformBlock {
        float viewProj[16];
        float view[16];        // Z-forward-adjusted view, for the foreground clip
        float cameraPos[4];
        float lightDir[4];     // .xyz = light direction, .w = clipFar (view-space; 0=off)
        float invViewProj[16]; // inverse(viewProj), for the skybox ray reconstruction
    };

    bool createRenderTargets();
    bool ensureTargets(uint32_t w, uint32_t h);   // (re)create color+depth+framebuffer at this size
    bool createPipeline();
    bool createSamplerAndDefaults();
    bool createIbl();   // generate BRDF LUT + irradiance + prefiltered cubes from the analytic sky
    ModelImage uploadTexture(const struct ModelTexture& tex);
    VkDescriptorSet makeMaterialSet(VkImageView baseColor, VkImageView mr,
                                    VkImageView normal, VkImageView occ,
                                    VkImageView emissive);
    bool finalizeModel(struct ModelData& md);   // upload geometry+textures, build material sets
    // Override the load-time (bind-pose) AABB with one measured from the active
    // animation: sample the clip, skin the verts on the CPU, union the box. The
    // bind pose lives in mesh space, which a re-orienting skeleton (e.g. a Z-up
    // mesh stood up Y-up) renders very differently — so the bind box gives the
    // wrong height/center for the fit. No-op when there's no active clip.
    void recomputeAnimatedBounds(const std::vector<ModelVertex>& verts,
                                 const std::vector<uint32_t>& indices);
    // Re-blend morphed primitives (base + Σ weightᵢ·deltaᵢ) into the host-visible
    // vertex buffer using each owning node's current weights. No-op without morph.
    // trackAnchor → also accumulate the morphed verts' world centroid (rig bind).
    void blendMorphs(bool trackAnchor = false);
    void updateUniforms(const float viewMatrix[16], const float projMatrix[16], float clipFar);
    void cleanupModel();

    // ── Core Vulkan handles (not owned, from OpenXR runtime) ─────────────
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool modelLoaded_ = false;
    std::string loadedModelPath_;
    uint32_t numPrimitives_ = 0;

    // True when the swapchain is an sRGB *format* (set per-frame from the
    // swapchainFormat passed to renderEye). The shader gamma-encodes its output
    // ONLY when this is false: a UNORM swapchain (Windows) needs the shader to
    // linear→sRGB encode, while an sRGB swapchain (macOS) gets the encode for
    // free from the blit's hardware write — encoding in the shader too would
    // double-encode. See pbr.frag / skybox.frag (ubo.cameraPos.w flag).
    bool swapchainIsSrgb_ = false;

    // ── Render targets (internal; blitted to the swapchain viewport) ─────
    VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    ModelImage colorImage_;
    ModelImage depthImage_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    // ── Pipeline ──────────────────────────────────────────────────────────
    VkDescriptorSetLayout dsLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline skyboxPipeline_ = VK_NULL_HANDLE;   // analytic-sky background (opaque mode)
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    ModelBuffer uniformBuffer_;   // host-visible UniformBlock

    // ── Material textures (set = 1: 5 combined image samplers) ───────────
    VkSampler sampler_ = VK_NULL_HANDLE;
    ModelImage whiteTex_;        // 1x1 white  — default base-color/MR/AO/emissive
    ModelImage flatNormalTex_;   // 1x1 (128,128,255) — default tangent-space normal
    VkDescriptorSetLayout matSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool matPool_ = VK_NULL_HANDLE;          // recreated per model
    std::vector<VkDescriptorSet> materialSets_;          // one per material
    VkDescriptorSet defaultMatSet_ = VK_NULL_HANDLE;     // for material == -1

    // ── IBL (set = 2: irradiance cube, prefiltered cube, BRDF LUT) ───────
    struct CubeMap {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // cube view (all mips/layers)
        uint32_t size = 0;
        uint32_t mips = 1;
    };
    // Render the analytic sky into each cube face/mip with the given fragment
    // SPIR-V; perMipRoughness pushes {face, roughness} (prefilter) vs {face} (irradiance).
    bool genCubeMap(CubeMap& cube, uint32_t size, uint32_t mips,
                    const uint32_t* fragSpv, size_t fragSpvBytes, bool perMipRoughness);
    ModelImage brdfLut_;                 // 2D R16G16_SFLOAT
    CubeMap irradianceCube_;
    CubeMap prefilterCube_;
    VkSampler iblCubeSampler_ = VK_NULL_HANDLE;
    VkSampler iblLutSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout iblSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool iblPool_ = VK_NULL_HANDLE;
    VkDescriptorSet iblSet_ = VK_NULL_HANDLE;

    // ── Skinning (set = 3: joint-matrix SSBO, vertex stage) ──────────────
    VkDescriptorSetLayout jointSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool jointPool_ = VK_NULL_HANDLE;       // recreated per model
    VkDescriptorSet jointSet_ = VK_NULL_HANDLE;
    ModelBuffer jointBuffer_;                            // host-visible mat4[] SSBO
    std::vector<ModelSkin> skins_;
    uint32_t jointCount_ = 0;                            // matrices in jointBuffer_

    // ── Morph targets (Phase 3: CPU blend into a host-visible vertex buffer) ─
    bool hasMorph_ = false;                  // → vertexBuffer_ is host-visible
    std::vector<ModelMorph>  morphs_;
    std::vector<ModelVertex> morphBase_;     // CPU base verts, re-blended per frame
    float morphCentroid_[3] = {0, 0, 0};     // raw world centroid of morphed verts
    bool  morphCentroidValid_ = false;       // (rig-bind fallback when no skeleton)

    // ── Loaded model GPU data ────────────────────────────────────────────
    ModelBuffer vertexBuffer_;
    ModelBuffer indexBuffer_;
    std::vector<ModelImage>     modelTextures_;
    std::vector<ModelMaterial>  materials_;
    std::vector<ModelPrimitive> primitives_;

    // ── Animation (Phase 1: node TRS). Empty graph → static fast-path ────────
    std::vector<ModelNode>  nodes_;
    std::vector<Animation>  animations_;
    std::vector<int>        rootNodes_;
    std::vector<float>      nodeWorld_;   // scratch: 16 floats/node, per-frame walk
    int   activeAnim_ = -1;              // -1 = none/static (fast-path guard)
    float animTime_   = 0.0f;            // playhead within the active clip (seconds)
    bool  paused_     = false;           // freeze the playhead (Phase 4 play/pause)
    std::vector<ModelNode> bindNodes_;   // bind-pose TRS snapshot; restored on clip switch

    // Display-rig bind: smoothed mean joint position (world space). Valid only
    // while a skinned model is animating; snaps on the first frame then eases.
    float animAnchor_[3] = {0, 0, 0};
    bool  animAnchorValid_ = false;

    // Correction added to the raw skeleton-centroid anchor so it lands on the
    // model's visual centre instead of the joint mean. = (animated AABB centre −
    // mean joint centroid), computed once over the clip in recomputeAnimatedBounds.
    // ~0 when the skeleton already spans the geometry (most glTF rigs); non-zero
    // when joint-free geometry sits off-centre (e.g. an FBX hat with no bones),
    // which would otherwise let the subject ride high/low in frame.
    float anchorOffset_[3] = {0, 0, 0};
    bool  anchorOffsetValid_ = false;

    float bboxMin_[3] = {0, 0, 0};
    float bboxMax_[3] = {0, 0, 0};
    bool  hasBBox_ = false;
};
