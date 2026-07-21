// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// stb_truetype + stb_image_write implementation TU for the Linux avatar build.
//
// main.cpp CPU-rasterizes the speech-bubble panel + text (the Local2D /
// XR_DXR_local_3d_zone layer) with stb_truetype — there is no CoreText /
// DirectWrite on Linux. The header comes from the nothings/stb FetchContent in
// CMakeLists.txt; define the implementation in exactly ONE TU (this one),
// mirroring linux/stb_image_impl.cpp's role for stb_image.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// stb_image_write for the optional DXR_DUMP_BUBBLE → /tmp/avatar_bubble.png
// debug dump. On Windows/macOS the stbi_write_png definition comes from
// displayxr::common (atlas_capture.cpp / stb_image_impl_macos.cpp), but the
// UNIX/Linux leg of displayxr-common compiles neither, so the symbol is
// otherwise unresolved at link. stb_image_write.h is on the include path via
// displayxr::common.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
