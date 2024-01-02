/*
 * Copyright 2022 Rive
 */

#pragma once

#include "rive/pls/gl/gl_state.hpp"
#include "rive/pls/pls_render_context_helper_impl.hpp"
#include "rive/pls/gl/pls_render_target_gl.hpp"
#include <map>

namespace rive::pls
{
class PLSPath;
class PLSPaint;
class PLSRenderTargetGL;

// OpenGL backend implementation of PLSRenderContextImpl.
class PLSRenderContextGLImpl : public PLSRenderContextHelperImpl
{
public:
    static std::unique_ptr<PLSRenderContext> MakeContext();
    ~PLSRenderContextGLImpl() override;

    // Called when the GL context has been modified outside of Rive.
    void resetGLState();

    // Creates a PLSRenderTarget that draws directly into the given GL framebuffer.
    // Returns null if the framebuffer doesn't support pixel local storage.
    rcp<PLSRenderTargetGL> wrapGLRenderTarget(GLuint framebufferID, uint32_t width, uint32_t height)
    {
        return m_plsImpl->wrapGLRenderTarget(framebufferID, width, height, m_platformFeatures);
    }

    // Creates a PLSRenderTarget that draws to a new, offscreen GL framebuffer. This method is
    // guaranteed to succeed, but the caller must call glBlitFramebuffer() to view the rendering
    // results.
    rcp<PLSRenderTargetGL> makeOffscreenRenderTarget(
        uint32_t width,
        uint32_t height,
        PLSRenderTargetGL::TargetTextureOwnership targetTextureOwnership =
            PLSRenderTargetGL::TargetTextureOwnership::internal)
    {
        return m_plsImpl->makeOffscreenRenderTarget(width,
                                                    height,
                                                    targetTextureOwnership,
                                                    m_platformFeatures);
    }

    rcp<RenderBuffer> makeRenderBuffer(RenderBufferType, RenderBufferFlags, size_t) override;

    rcp<PLSTexture> makeImageTexture(uint32_t width,
                                     uint32_t height,
                                     uint32_t mipLevelCount,
                                     const uint8_t imageDataRGBA[]) override;

    GLState* state() const { return m_state.get(); }

private:
    class DrawProgram;

    // Manages how we implement pixel local storage in shaders.
    class PLSImpl
    {
    public:
        virtual void init(rcp<GLState>) {}

        virtual rcp<PLSRenderTargetGL> wrapGLRenderTarget(GLuint framebufferID,
                                                          uint32_t width,
                                                          uint32_t height,
                                                          const PlatformFeatures&) = 0;
        virtual rcp<PLSRenderTargetGL> makeOffscreenRenderTarget(
            uint32_t width,
            uint32_t height,
            PLSRenderTargetGL::TargetTextureOwnership,
            const PlatformFeatures&) = 0;

        virtual void activatePixelLocalStorage(PLSRenderContextGLImpl*, const FlushDescriptor&) = 0;
        virtual void deactivatePixelLocalStorage(PLSRenderContextGLImpl*) = 0;

        virtual const char* shaderDefineName() const = 0;

        void ensureRasterOrderingEnabled(bool enabled)
        {
            if (m_rasterOrderingEnabled != enabled)
            {
                onBarrier();
                onEnableRasterOrdering(enabled);
                m_rasterOrderingEnabled = enabled;
            }
        }

        virtual void barrier()
        {
            assert(!m_rasterOrderingEnabled);
            onBarrier();
        }

        virtual ~PLSImpl() {}

    private:
        virtual void onEnableRasterOrdering(bool enabled) {}
        virtual void onBarrier() {}

        bool m_rasterOrderingEnabled = true;
    };

    class PLSImplEXTNative;
    class PLSImplFramebufferFetch;
    class PLSImplWebGL;
    class PLSImplRWTexture;

    static std::unique_ptr<PLSImpl> MakePLSImplEXTNative(const GLExtensions&);
    static std::unique_ptr<PLSImpl> MakePLSImplFramebufferFetch(const GLExtensions&);
    static std::unique_ptr<PLSImpl> MakePLSImplWebGL();
    static std::unique_ptr<PLSImpl> MakePLSImplRWTexture();

    static std::unique_ptr<PLSRenderContext> MakeContext(const char* rendererString,
                                                         GLExtensions,
                                                         std::unique_ptr<PLSImpl>);

    PLSRenderContextGLImpl(const char* rendererString, GLExtensions, std::unique_ptr<PLSImpl>);

    // Wraps a compiled and linked GL program of draw_path.glsl or draw_image_mesh.glsl, with a
    // specific set of features enabled via #define. The set of features to enable is dictated by
    // ShaderFeatures.
    class DrawProgram
    {
    public:
        DrawProgram(const DrawProgram&) = delete;
        DrawProgram& operator=(const DrawProgram&) = delete;
        DrawProgram(PLSRenderContextGLImpl*, DrawType, ShaderFeatures, pls::InterlockMode);
        ~DrawProgram();

        GLuint id() const { return m_id; }
        GLint spirvCrossBaseInstanceLocation() const { return m_spirvCrossBaseInstanceLocation; }

    private:
        GLuint m_id;
        GLint m_spirvCrossBaseInstanceLocation = -1;
        const rcp<GLState> m_state;
    };

    class DrawShader;

    std::unique_ptr<BufferRing> makeVertexBufferRing(size_t capacity,
                                                     size_t itemSizeInBytes) override;

    std::unique_ptr<BufferRing> makePixelUnpackBufferRing(size_t capacity,
                                                          size_t itemSizeInBytes) override;

    std::unique_ptr<BufferRing> makeUniformBufferRing(size_t capacity, size_t sizeInBytes) override;

    void resizePathTexture(uint32_t width, uint32_t height) override;
    void resizeContourTexture(uint32_t width, uint32_t height) override;
    void resizeGradientTexture(uint32_t width, uint32_t height) override;
    void resizeTessellationTexture(uint32_t width, uint32_t height) override;

    void flush(const FlushDescriptor&) override;

    GLExtensions m_extensions;

    constexpr static size_t kShaderVersionStringBuffSize = sizeof("#version 300 es\n") + 1;
    char m_shaderVersionString[kShaderVersionStringBuffSize];

    std::unique_ptr<PLSImpl> m_plsImpl;

    // Path/contour data textures.
    GLuint m_pathTexture = 0;
    GLuint m_contourTexture = 0;

    // Gradient texture rendering.
    GLuint m_colorRampProgram;
    GLuint m_colorRampVAO;
    GLuint m_colorRampFBO;
    GLuint m_gradientTexture = 0;

    // Tessellation texture rendering.
    GLuint m_tessellateProgram;
    GLuint m_tessellateVAO;
    GLuint m_tessSpanIndexBuffer;
    GLuint m_tessellateFBO;
    GLuint m_tessVertexTexture = 0;

    // Not all programs have a unique vertex shader, so we cache and reuse them where possible.
    std::map<uint32_t, DrawShader> m_vertexShaders;
    std::map<uint32_t, DrawProgram> m_drawPrograms;

    // Vertex/index buffers for drawing paths.
    GLuint m_drawVAO = 0;
    GLuint m_patchVerticesBuffer = 0;
    GLuint m_patchIndicesBuffer = 0;
    GLuint m_interiorTrianglesVAO = 0;

    // Vertex/index buffers for drawing image rects. (Atomic mode only, and only used when bindless
    // textures aren't supported.)
    GLuint m_imageRectVAO = 0;
    GLuint m_imageRectVertexBuffer = 0;
    GLuint m_imageRectIndexBuffer = 0;

    GLuint m_imageMeshVAO = 0;
    GLuint m_plsResolveVAO = 0;

    // Extra buffers for the experimental atomic mode.
    GLuint m_paintBuffer = 0;
    GLuint m_paintMatrixBuffer = 0;
    GLuint m_paintTranslateBuffer = 0;
    GLuint m_clipRectMatrixBuffer = 0;
    GLuint m_clipRectTranslateBuffer = 0;

    const rcp<GLState> m_state;
};
} // namespace rive::pls
