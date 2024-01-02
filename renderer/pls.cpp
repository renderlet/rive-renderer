/*
 * Copyright 2022 Rive
 */

#include "rive/pls/pls.hpp"

#include "rive/pls/pls_render_target.hpp"
#include "shaders/constants.glsl"

#include "../out/obj/generated/draw_path.exports.h"

namespace rive::pls
{
static_assert(kGradTextureWidth == GRAD_TEXTURE_WIDTH);
static_assert(kTessTextureWidth == TESS_TEXTURE_WIDTH);
static_assert(kTessTextureWidthLog2 == TESS_TEXTURE_WIDTH_LOG2);

const char* GetShaderFeatureGLSLName(ShaderFeatures feature)
{
    switch (feature)
    {
        case ShaderFeatures::NONE:
            RIVE_UNREACHABLE();
        case ShaderFeatures::ENABLE_CLIPPING:
            return GLSL_ENABLE_CLIPPING;
        case ShaderFeatures::ENABLE_CLIP_RECT:
            return GLSL_ENABLE_CLIP_RECT;
        case ShaderFeatures::ENABLE_ADVANCED_BLEND:
            return GLSL_ENABLE_ADVANCED_BLEND;
        case ShaderFeatures::ENABLE_EVEN_ODD:
            return GLSL_ENABLE_EVEN_ODD;
        case ShaderFeatures::ENABLE_NESTED_CLIPPING:
            return GLSL_ENABLE_NESTED_CLIPPING;
        case ShaderFeatures::ENABLE_HSL_BLEND_MODES:
            return GLSL_ENABLE_HSL_BLEND_MODES;
    }
    RIVE_UNREACHABLE();
}

constexpr static float pack_params(int32_t patchSegmentSpan, int32_t vertexType)
{
    return static_cast<float>((patchSegmentSpan << 2) | vertexType);
}

static void generate_buffer_data_for_patch_type(PatchType patchType,
                                                PatchVertex vertices[],
                                                uint16_t indices[],
                                                uint16_t baseVertex)
{
    // AA border vertices. "Inner tessellation curves" have one more segment without a fan triangle
    // whose purpose is to be a bowtie join.
    size_t vertexCount = 0;
    size_t patchSegmentSpan = patchType == PatchType::midpointFan ? kMidpointFanPatchSegmentSpan
                                                                  : kOuterCurvePatchSegmentSpan;
    for (int i = 0; i < patchSegmentSpan; ++i)
    {
        float params = pack_params(patchSegmentSpan, STROKE_VERTEX);
        float l = static_cast<float>(i);
        float r = l + 1;
        if (patchType == PatchType::outerCurves)
        {
            vertices[vertexCount + 0].set(l, 0.f, .5f, params);
            vertices[vertexCount + 1].set(l, 1.f, .0f, params);
            vertices[vertexCount + 2].set(r, 0.f, .5f, params);
            vertices[vertexCount + 3].set(r, 1.f, .0f, params);

            // Give the vertex an alternate position when mirrored so the border has the same
            // diagonals whether morrored or not.
            vertices[vertexCount + 0].setMirroredPosition(r, 0.f, .5f);
            vertices[vertexCount + 1].setMirroredPosition(l, 0.f, .5f);
            vertices[vertexCount + 2].setMirroredPosition(r, 1.f, .0f);
            vertices[vertexCount + 3].setMirroredPosition(l, 1.f, .0f);
        }
        else
        {
            assert(patchType == PatchType::midpointFan);
            vertices[vertexCount + 0].set(l, -1.f, 1.f, params);
            vertices[vertexCount + 1].set(l, +1.f, 0.f, params);
            vertices[vertexCount + 2].set(r, -1.f, 1.f, params);
            vertices[vertexCount + 3].set(r, +1.f, 0.f, params);

            // Give the vertex an alternate position when mirrored so the border has the same
            // diagonals whether morrored or not.
            vertices[vertexCount + 0].setMirroredPosition(r - 1.f, -1.f, 1.f);
            vertices[vertexCount + 1].setMirroredPosition(l - 1.f, -1.f, 1.f);
            vertices[vertexCount + 2].setMirroredPosition(r - 1.f, +1.f, 0.f);
            vertices[vertexCount + 3].setMirroredPosition(l - 1.f, +1.f, 0.f);
        }
        vertexCount += 4;
    }

    // Bottom (negative coverage) side of the AA border.
    if (patchType == PatchType::outerCurves)
    {
        float params = pack_params(patchSegmentSpan, STROKE_VERTEX);
        for (int i = 0; i < patchSegmentSpan; ++i)
        {
            float l = static_cast<float>(i);
            float r = l + 1;

            vertices[vertexCount + 0].set(l, -.0f, .5f, params);
            vertices[vertexCount + 1].set(r, -.0f, .5f, params);
            vertices[vertexCount + 2].set(l, -1.f, .0f, params);
            vertices[vertexCount + 3].set(r, -1.f, .0f, params);

            // Give the vertex an alternate position when mirrored so the border has the same
            // diagonals whether morrored or not.
            vertices[vertexCount + 0].setMirroredPosition(r, -0.f, .5f);
            vertices[vertexCount + 1].setMirroredPosition(r, -1.f, .0f);
            vertices[vertexCount + 2].setMirroredPosition(l, -0.f, .5f);
            vertices[vertexCount + 3].setMirroredPosition(l, -1.f, .0f);

            vertexCount += 4;
        }
    }

    // Triangle fan vertices. (These only touch the first "fanSegmentSpan" segments on inner
    // tessellation curves.
    size_t fanVerticesIdx = vertexCount;
    size_t fanSegmentSpan =
        patchType == PatchType::midpointFan ? patchSegmentSpan : patchSegmentSpan - 1;
    assert((fanSegmentSpan & (fanSegmentSpan - 1)) == 0); // The fan must be a power of two.
    for (int i = 0; i <= fanSegmentSpan; ++i)
    {
        float params = pack_params(patchSegmentSpan, FAN_VERTEX);
        if (patchType == PatchType::outerCurves)
        {
            vertices[vertexCount].set(static_cast<float>(i), 0.f, 1, params);
        }
        else
        {
            vertices[vertexCount].set(static_cast<float>(i), -1.f, 1, params);
            vertices[vertexCount].setMirroredPosition(static_cast<float>(i) - 1, -1.f, 1);
        }
        ++vertexCount;
    }

    // The midpoint vertex is only included on midpoint fan patches.
    size_t midpointIdx = vertexCount;
    if (patchType == PatchType::midpointFan)
    {
        vertices[vertexCount++].set(0, 0, 1, pack_params(patchSegmentSpan, FAN_MIDPOINT_VERTEX));
    }
    assert(vertexCount == (patchType == PatchType::outerCurves ? kOuterCurvePatchVertexCount
                                                               : kMidpointFanPatchVertexCount));

    // AA border indices.
    constexpr static size_t kBorderPatternVertexCount = 4;
    constexpr static size_t kBorderPatternIndexCount = 6;
    constexpr static uint16_t kBorderPattern[kBorderPatternIndexCount] = {0, 1, 2, 2, 1, 3};
    constexpr static uint16_t kNegativeBorderPattern[kBorderPatternIndexCount] = {0, 2, 1, 1, 2, 3};

    size_t indexCount = 0;
    size_t borderEdgeVerticesIdx = 0;
    for (size_t borderSegmentIdx = 0; borderSegmentIdx < patchSegmentSpan; ++borderSegmentIdx)
    {
        for (size_t i = 0; i < kBorderPatternIndexCount; ++i)
        {
            indices[indexCount++] = baseVertex + borderEdgeVerticesIdx + kBorderPattern[i];
        }
        borderEdgeVerticesIdx += kBorderPatternVertexCount;
    }

    // Bottom (negative coverage) side of the AA border.
    if (patchType == PatchType::outerCurves)
    {
        for (size_t borderSegmentIdx = 0; borderSegmentIdx < patchSegmentSpan; ++borderSegmentIdx)
        {
            for (size_t i = 0; i < kBorderPatternIndexCount; ++i)
            {
                indices[indexCount++] =
                    baseVertex + borderEdgeVerticesIdx + kNegativeBorderPattern[i];
            }
            borderEdgeVerticesIdx += kBorderPatternVertexCount;
        }
    }

    assert(borderEdgeVerticesIdx == fanVerticesIdx);

    // Triangle fan indices, in a middle-out topology.
    // Don't include the final bowtie join if this is an "outerStroke" patch. (i.e., use
    // fanSegmentSpan and not "patchSegmentSpan".)
    for (int step = 1; step < fanSegmentSpan; step <<= 1)
    {
        for (int i = 0; i < fanSegmentSpan; i += step * 2)
        {
            indices[indexCount++] = fanVerticesIdx + i + baseVertex;
            indices[indexCount++] = fanVerticesIdx + i + step + baseVertex;
            indices[indexCount++] = fanVerticesIdx + i + step * 2 + baseVertex;
        }
    }
    if (patchType == PatchType::midpointFan)
    {
        // Triangle to the contour midpoint.
        indices[indexCount++] = fanVerticesIdx + baseVertex;
        indices[indexCount++] = fanVerticesIdx + fanSegmentSpan + baseVertex;
        indices[indexCount++] = midpointIdx + baseVertex;
        assert(indexCount == kMidpointFanPatchIndexCount);
    }
    else
    {
        assert(patchType == PatchType::outerCurves);
        assert(indexCount == kOuterCurvePatchIndexCount);
    }
}

void GeneratePatchBufferData(PatchVertex vertices[kPatchVertexBufferCount],
                             uint16_t indices[kPatchIndexBufferCount])
{
    generate_buffer_data_for_patch_type(PatchType::midpointFan, vertices, indices, 0);
    generate_buffer_data_for_patch_type(PatchType::outerCurves,
                                        vertices + kMidpointFanPatchVertexCount,
                                        indices + kMidpointFanPatchIndexCount,
                                        kMidpointFanPatchVertexCount);
}

float FindTransformedArea(const AABB& bounds, const Mat2D& matrix)
{
    Vec2D pts[4] = {{bounds.left(), bounds.top()},
                    {bounds.right(), bounds.top()},
                    {bounds.right(), bounds.bottom()},
                    {bounds.left(), bounds.bottom()}};
    Vec2D screenSpacePts[4];
    matrix.mapPoints(screenSpacePts, pts, 4);
    Vec2D v[3] = {screenSpacePts[1] - screenSpacePts[0],
                  screenSpacePts[2] - screenSpacePts[0],
                  screenSpacePts[3] - screenSpacePts[0]};
    return (fabsf(Vec2D::cross(v[0], v[1])) + fabsf(Vec2D::cross(v[1], v[2]))) * .5f;
}

void ClipRectInverseMatrix::reset(const Mat2D& clipMatrix, const AABB& clipRect)
{
    // Find the matrix that transforms from pixel space to "normalized clipRect space", where the
    // clipRect is the normalized rectangle: [-1, -1, +1, +1].
    Mat2D m = clipMatrix * Mat2D(clipRect.width() * .5f,
                                 0,
                                 0,
                                 clipRect.height() * .5f,
                                 clipRect.center().x,
                                 clipRect.center().y);
    if (clipRect.width() <= 0 || clipRect.height() <= 0 || !m.invert(&m_inverseMatrix))
    {
        // If the width or height went zero or negative, or if "m" is non-invertible, clip away
        // everything.
        *this = Empty();
    }
}

static uint32_t paint_type_to_glsl_id(PaintType paintType)
{
    return static_cast<uint32_t>(paintType);
    static_assert((int)PaintType::solidColor == SOLID_COLOR_PAINT_TYPE);
    static_assert((int)PaintType::linearGradient == LINEAR_GRADIENT_PAINT_TYPE);
    static_assert((int)PaintType::radialGradient == RADIAL_GRADIENT_PAINT_TYPE);
    static_assert((int)PaintType::image == IMAGE_PAINT_TYPE);
    static_assert((int)PaintType::clipUpdate == CLIP_UPDATE_PAINT_TYPE);
}

uint32_t ConvertBlendModeToPLSBlendMode(BlendMode riveMode)
{
    switch (riveMode)
    {
        case BlendMode::srcOver:
            return BLEND_SRC_OVER;
        case BlendMode::screen:
            return BLEND_MODE_SCREEN;
        case BlendMode::overlay:
            return BLEND_MODE_OVERLAY;
        case BlendMode::darken:
            return BLEND_MODE_DARKEN;
        case BlendMode::lighten:
            return BLEND_MODE_LIGHTEN;
        case BlendMode::colorDodge:
            return BLEND_MODE_COLORDODGE;
        case BlendMode::colorBurn:
            return BLEND_MODE_COLORBURN;
        case BlendMode::hardLight:
            return BLEND_MODE_HARDLIGHT;
        case BlendMode::softLight:
            return BLEND_MODE_SOFTLIGHT;
        case BlendMode::difference:
            return BLEND_MODE_DIFFERENCE;
        case BlendMode::exclusion:
            return BLEND_MODE_EXCLUSION;
        case BlendMode::multiply:
            return BLEND_MODE_MULTIPLY;
        case BlendMode::hue:
            return BLEND_MODE_HUE;
        case BlendMode::saturation:
            return BLEND_MODE_SATURATION;
        case BlendMode::color:
            return BLEND_MODE_COLOR;
        case BlendMode::luminosity:
            return BLEND_MODE_LUMINOSITY;
    }
    RIVE_UNREACHABLE();
}

void PathData::set(const Mat2D& m,
                   float strokeRadius_,
                   FillRule fillRule,
                   PaintType paintType,
                   uint32_t clipID,
                   BlendMode riveBlendMode,
                   const PaintData& paintData_,
                   const ClipRectInverseMatrix* clipRectInverseMatrix_)
{
    matrix = m;
    strokeRadius = strokeRadius_;
    uint32_t localParams = ConvertBlendModeToPLSBlendMode(riveBlendMode);
    localParams |= clipID << 4;
    localParams |= paint_type_to_glsl_id(paintType) << 20;
    if (fillRule == FillRule::evenOdd && strokeRadius_ == 0)
    {
        localParams |= EVEN_ODD_PATH_FLAG;
    }
    params = localParams;
    paintData = paintData_;
    clipRectInverseMatrix = clipRectInverseMatrix_ != nullptr ? *clipRectInverseMatrix_
                                                              : ClipRectInverseMatrix::WideOpen();
}

ImageDrawUniforms::ImageDrawUniforms(const Mat2D& matrix_,
                                     float opacity_,
                                     const ClipRectInverseMatrix* clipRectInverseMatrix_,
                                     uint32_t clipID_,
                                     BlendMode blendMode_) :
    matrix(matrix_),
    opacity(opacity_),
    clipRectInverseMatrix(clipRectInverseMatrix_ != nullptr ? *clipRectInverseMatrix_
                                                            : ClipRectInverseMatrix::WideOpen()),
    clipID(clipID_),
    blendMode(ConvertBlendModeToPLSBlendMode(blendMode_))
{}

static uint32_t pack_glsl_unorm4x8(uint4 rgbaInteger)
{
    rgbaInteger <<= uint4{0, 8, 16, 24};
    rgbaInteger.xy |= rgbaInteger.zw;
    rgbaInteger.x |= rgbaInteger.y;
    return rgbaInteger.x;
}

static uint32_t pack_glsl_unorm4x8(float4 rgba)
{
    uint4 rgbaInteger = simd::cast<uint32_t>(simd::clamp(rgba, float4(0), float4(1)) * 255.f);
    return pack_glsl_unorm4x8(rgbaInteger);
}

static uint32_t pack_glsl_unorm4x8(ColorInt riveColor)
{
    // Swizzle the riveColor to the order GLSL expects in unpackUnorm4x8().
    uint4 rgbaInteger = (uint4(riveColor) >> uint4{16, 8, 0, 24}) & 0xffu;
    return pack_glsl_unorm4x8(rgbaInteger);
}

void ExperimentalAtomicModeData::setDataForPath(uint32_t pathID,
                                                const Mat2D& matrix,
                                                FillRule fillRule,
                                                PaintType paintType,
                                                const PaintData& paintData,
                                                const PLSTexture* imageTexture,
                                                uint32_t clipID,
                                                const ClipRectInverseMatrix* clipRectInverseMatrix,
                                                BlendMode blendMode,
                                                const PLSRenderTarget* renderTarget,
                                                float gradientTextureHeight,
                                                const PlatformFeatures& platformFeatures)
{
    uint32_t shiftedClipID = clipID << 16;
    uint32_t shiftedBlendMode = ConvertBlendModeToPLSBlendMode(blendMode) << 4;
    m_imageTextures[pathID] = nullptr;
    switch (paintType)
    {
        case PaintType::solidColor:
        {
            m_paints[pathID].params = shiftedClipID | shiftedBlendMode | SOLID_COLOR_PAINT_TYPE;
            m_paints[pathID].color = pack_glsl_unorm4x8(simd::load4f(&paintData.data));
            break;
        }
        case PaintType::linearGradient:
        case PaintType::radialGradient:
        case PaintType::image:
        {
            Mat2D paintMatrix;
            matrix.invert(&paintMatrix);
            if (platformFeatures.fragCoordBottomUp)
            {
                // Flip gl_FragCoord.y.
                paintMatrix = paintMatrix * Mat2D(1, 0, 0, -1, 0, renderTarget->height());
            }
            if (paintType == PaintType::image)
            {
                m_paints[pathID].params = shiftedClipID | shiftedBlendMode | IMAGE_PAINT_TYPE;
                m_paints[pathID].opacity = paintData.opacityIfImage();
                m_imageTextures[pathID] = imageTexture;
            }
            else
            {
                float gradCoeffs[3];
                memcpy(gradCoeffs, paintData.data + 1, 3 * sizeof(float));
                if (paintType == PaintType::linearGradient)
                {
                    m_paints[pathID].params =
                        shiftedClipID | shiftedBlendMode | LINEAR_GRADIENT_PAINT_TYPE;
                    paintMatrix =
                        Mat2D(gradCoeffs[0], 0, gradCoeffs[1], 0, gradCoeffs[2], 0) * paintMatrix;
                }
                else
                {
                    m_paints[pathID].params =
                        shiftedClipID | shiftedBlendMode | RADIAL_GRADIENT_PAINT_TYPE;
                    float w = 1 / gradCoeffs[2];
                    paintMatrix =
                        Mat2D(w, 0, 0, w, -gradCoeffs[0] * w, -gradCoeffs[1] * w) * paintMatrix;
                }
                uint32_t span = paintData.data[0];
                uint32_t row = span >> 20;
                uint32_t x0 = span & 0x3ff;
                uint32_t x1 = (span >> 10) & 0x3ff;
                m_paints[pathID].gradTextureY = static_cast<float>(row) + .5f;
                // Generate a mapping from gradient T to an x-coord in the gradient texture.
                m_paintTranslates[pathID].gradTextureHorizontalSpan = {
                    (x1 - x0) * GRAD_TEXTURE_INVERSE_WIDTH,
                    (x0 + .5f) * GRAD_TEXTURE_INVERSE_WIDTH};
            }
            m_paintMatrices[pathID] = {paintMatrix.xx(),
                                       paintMatrix.xy(),
                                       paintMatrix.yx(),
                                       paintMatrix.yy()};
            m_paintTranslates[pathID].translate = {paintMatrix.tx(), paintMatrix.ty()};
            break;
        }
        case PaintType::clipUpdate:
        {
            uint32_t outerClipID = paintData.outerClipIDIfClipUpdate();
            m_paints[pathID].params = (outerClipID << 16) | CLIP_UPDATE_PAINT_TYPE;
            m_paints[pathID].shiftedClipReplacementID = shiftedClipID;
            break;
        }
    }
    if (fillRule == FillRule::evenOdd)
    {
        m_paints[pathID].params |= ATOMIC_MODE_FLAG_EVEN_ODD;
    }
    if (clipRectInverseMatrix != nullptr)
    {
        m_paints[pathID].params |= ATOMIC_MODE_FLAG_HAS_CLIP_RECT;
        Mat2D m = clipRectInverseMatrix->inverseMatrix();
        if (platformFeatures.fragCoordBottomUp)
        {
            // Flip gl_FragCoord.y.
            m = m * Mat2D(1, 0, 0, -1, 0, renderTarget->height());
        }
        m_clipRectMatrices[pathID] = {m.xx(), m.xy(), m.yx(), m.yy()};
        m_clipRectTranslates[pathID].translate = {m.tx(), m.ty()};
        m_clipRectTranslates[pathID].inverseFwidth = {-1.f / (fabsf(m.xx()) + fabsf(m.xy())),
                                                      -1.f / (fabsf(m.yx()) + fabsf(m.yy()))};
    }
}

void ExperimentalAtomicModeData::finalize(size_t pathCount,
                                          ColorInt clearColor,
                                          float gradTextureInverseHeight)
{
    // Configure pathID 0 to resolve to the clear color, so we can avoid clearing the
    // framebuffer in certain cases.
    m_paints[0].params = SOLID_COLOR_PAINT_TYPE;
    m_paints[0].color = pack_glsl_unorm4x8(clearColor);
    m_imageTextures[0] = nullptr;

    // Normalize the gradient Y coordinates now that we know how tall the texture is.
    for (size_t i = 1; i <= pathCount; ++i)
    {
        uint32_t paintType = m_paints[i].params & 0xfu;
        if (paintType == LINEAR_GRADIENT_PAINT_TYPE || paintType == RADIAL_GRADIENT_PAINT_TYPE)
        {
            m_paints[i].gradTextureY *= gradTextureInverseHeight;
        }
    }
}

} // namespace rive::pls
