// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// stb_image implementation TU. The glTF loader builds tinygltf with
// TINYGLTF_NO_STB_IMAGE and supplies a custom image loader that calls stbi_*;
// on desktop the stb implementation comes from displayxr::common, which the
// Android leg doesn't link — so define it here once. stb_image.h is supplied
// by tinygltf (FetchContent) on the include path.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// stb_truetype for the button-bar glyph bake (hud_bar.cpp). The header comes
// from the stb FetchContent in CMakeLists.txt.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
