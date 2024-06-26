/*
 * Copyright 2023 Rive
 */

// This file includes the Metal Objective-C backend's headers in a way that the obfuscator can find
// their symbols without needing to support Objective-C. We currently use castxml, which does not
// support Objective-C.

#include <cstddef>

#define RIVE_OBJC_NOP

template <typename T> struct id
{
    id() = default;
    id(std::nullptr_t) {}
};

using MTLDevice = void*;
using MTLCommandQueue = void*;
using MTLLibrary = void*;
using MTLTexture = void*;
using MTLBuffer = void*;
using MTLPixelFormat = void*;
using MTLTextureUsage = void*;

#include "rive/pls/metal/pls_render_context_metal_impl.h"

namespace rive::pls
{
const char* rive_pls_macosx_metallib;
int rive_pls_macosx_metallib_len;
const char* rive_pls_ios_metallib;
int rive_pls_ios_metallib_len;
const char* rive_pls_ios_simulator_metallib;
int rive_pls_ios_simulator_metallib_len;
void make_memoryless_pls_texture();
void make_pipeline_state();
} // namespace rive::pls
