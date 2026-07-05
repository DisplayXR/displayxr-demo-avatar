// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// stb_image implementation TU for the Linux avatar build. model_loader.cpp
// (model_common/) only *declares* stb_image: on macOS the definition comes from
// displayxr::common's stb_image_impl_macos.cpp, which is macOS-gated, and on
// Windows from the D3D11 renderer TU. The Linux build must supply its own, else
// stbi_load / stbi_image_free are undefined at link. Mirrors the runtime's
// cube_handle_vk_linux/stb_image_impl.cpp (#21, #660).
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
