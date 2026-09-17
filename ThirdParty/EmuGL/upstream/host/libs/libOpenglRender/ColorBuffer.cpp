/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "ColorBuffer.h"

#include "EGLDispatch.h"
#include "GraphicsDiagnostics.h"
#include "GLESv1Dispatch.h"
#include "GLcommon/GLutils.h"
#include "GLESv2Dispatch.h"
#include "RenderThreadInfo.h"
#include "TextureDraw.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <atomic>

namespace {

// Lazily create and bind a framebuffer object to the current host context.
// |fbo| is the address of the framebuffer object name.
// |tex| is the name of a texture that is attached to the framebuffer object
// on creation only. I.e. all rendering operations will target it.
// returns true in case of success, false on failure.
bool bindFbo(GLuint* fbo, GLuint tex) {
    if (*fbo) {
        // fbo already exist - just bind
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        return true;
    }

    s_gles2.glGenFramebuffers(1, fbo);
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    s_gles2.glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0_OES,
                                   GL_TEXTURE_2D, tex, 0);
    GLenum status = s_gles2.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE_OES) {
        ERR("ColorBuffer::bindFbo: FBO not complete: %#x\n", status);
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        s_gles2.glDeleteFramebuffers(1, fbo);
        *fbo = 0;
        return false;
    }
    return true;
}

void unbindFbo() {
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool diagnosticReadTextureRGBA(GLuint texture,
                               GLuint width,
                               GLuint height,
                               std::vector<unsigned char> *pixels,
                               GLenum *statusOut = NULL) {
    if (!pixels || !texture || !width || !height) return false;
    GLint previousFbo = 0;
    GLint previousPack = 4;
    s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    s_gles2.glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
    GLuint fbo = 0;
    s_gles2.glGenFramebuffers(1, &fbo);
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    s_gles2.glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, texture, 0);
    const GLenum status = s_gles2.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (statusOut) *statusOut = status;
    bool ok = status == GL_FRAMEBUFFER_COMPLETE_OES;
    if (ok) {
        pixels->resize(static_cast<size_t>(width) * height * 4);
        s_gles2.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        s_gles2.glReadPixels(0, 0, width, height,
                             GL_RGBA, GL_UNSIGNED_BYTE, pixels->data());
    }
    s_gles2.glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, previousFbo);
    s_gles2.glDeleteFramebuffers(1, &fbo);
    return ok;
}

// Helper class to use a ColorBuffer::Helper context.
// Usage is pretty simple:
//
//     {
//        ScopedHelperContext context(m_helper);
//        if (!context.isOk()) {
//            return false;   // something bad happened.
//        }
//        .... do something ....
//     }   // automatically calls m_helper->teardownContext();
//
class ScopedHelperContext {
public:
    ScopedHelperContext(ColorBuffer::Helper* helper) : mHelper(helper) {
        if (!helper->setupContext()) {
            mHelper = NULL;
        }
    }

    bool isOk() const { return mHelper != NULL; }

    ~ScopedHelperContext() {
        release();
    }

    void release() {
        if (mHelper) {
            mHelper->teardownContext();
            mHelper = NULL;
        }
    }
private:
    ColorBuffer::Helper* mHelper;
};

}  // namespace

// static
ColorBuffer* ColorBuffer::create(EGLDisplay p_display,
                                 int p_width,
                                 int p_height,
                                 GLenum p_internalFormat,
                                 bool has_eglimage_texture_2d,
                                 Helper* helper) {
    GLenum texInternalFormat = 0;

    switch (p_internalFormat) {
        case GL_RGB:
        case GL_RGB565_OES:
            texInternalFormat = GL_RGB;
            break;

        case GL_RGBA:
        case GL_RGB5_A1_OES:
        case GL_RGBA4_OES:
            texInternalFormat = GL_RGBA;
            break;

        default:
            return NULL;
            break;
    }

    ScopedHelperContext context(helper);
    if (!context.isOk()) {
        return NULL;
    }

    ColorBuffer *cb = new ColorBuffer(p_display, helper);

    s_gles2.glGenTextures(1, &cb->m_tex);
    s_gles2.glBindTexture(GL_TEXTURE_2D, cb->m_tex);

    int nComp = (texInternalFormat == GL_RGB ? 3 : 4);

    char* zBuff = static_cast<char*>(::calloc(nComp * p_width * p_height, 1));
    if (!zBuff) { delete cb; return NULL; }
    s_gles2.glTexImage2D(GL_TEXTURE_2D,
                         0,
                         texInternalFormat,
                         p_width,
                         p_height,
                         0,
                         texInternalFormat,
                         GL_UNSIGNED_BYTE,
                         zBuff);

    const GLint colorFilter = aeGraphicsDiagEnabled("AE_DIAG_NEAREST_COLORBUFFER_FILTER")
            ? GL_NEAREST : GL_LINEAR;
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, colorFilter);
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, colorFilter);
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    //
    // create another texture for that colorbuffer for blit
    //
    s_gles2.glGenTextures(1, &cb->m_blitTex);
    s_gles2.glBindTexture(GL_TEXTURE_2D, cb->m_blitTex);
    s_gles2.glTexImage2D(GL_TEXTURE_2D,
                         0,
                         texInternalFormat,
                         p_width,
                         p_height,
                         0,
                         texInternalFormat,
                         GL_UNSIGNED_BYTE,
                         zBuff);
    ::free(zBuff);

    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    cb->m_width = p_width;
    cb->m_height = p_height;
    cb->m_internalFormat = texInternalFormat;

    if (has_eglimage_texture_2d) {
        const EGLint preservedImageAttributes[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE
        };
        const EGLint defaultImageAttributes[] = {EGL_NONE};
        const EGLint *imageAttributes = aeGraphicsDiagEnabled("AE_DIAG_DISABLE_EGLIMAGE_PRESERVED")
                ? defaultImageAttributes : preservedImageAttributes;
        cb->m_eglImage = s_egl.eglCreateImageKHR(
                p_display,
                s_egl.eglGetCurrentContext(),
                EGL_GL_TEXTURE_2D_KHR,
                (EGLClientBuffer)SafePointerFromUInt(cb->m_tex),
                imageAttributes);

        cb->m_blitEGLImage = s_egl.eglCreateImageKHR(
                p_display,
                s_egl.eglGetCurrentContext(),
                EGL_GL_TEXTURE_2D_KHR,
                (EGLClientBuffer)SafePointerFromUInt(cb->m_blitTex),
                imageAttributes);
        if (!cb->m_eglImage || !cb->m_blitEGLImage) { delete cb; return NULL; }
    }
#ifdef AE_SYNC_SHARED_IMAGES
    if (!aeGraphicsDiagEnabled("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH")) {
        s_gles2.glFinish();
    }
#endif
    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog(
                "CB_CREATE",
                "cb=%p size=%dx%d internal=%#x tex=%u blitTex=%u eglImage=%p blitImage=%p preserveAttr=%d",
                cb, p_width, p_height, p_internalFormat,
                cb->m_tex, cb->m_blitTex,
                cb->m_eglImage, cb->m_blitEGLImage,
                !aeGraphicsDiagEnabled("AE_DIAG_DISABLE_EGLIMAGE_PRESERVED"));
    }
    return cb;
}

ColorBuffer::ColorBuffer(EGLDisplay display, Helper* helper) :
        m_tex(0),
        m_blitTex(0),
        m_eglImage(NULL),
        m_blitEGLImage(NULL),
        m_fbo(0),
        m_internalFormat(0),
        m_display(display),
        m_helper(helper) {}

ColorBuffer::~ColorBuffer() {
    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("CB_DESTROY", "cb=%p tex=%u blitTex=%u", this, m_tex, m_blitTex);
    }
    ScopedHelperContext context(m_helper);

    if (m_blitEGLImage) {
        s_egl.eglDestroyImageKHR(m_display, m_blitEGLImage);
    }
    if (m_eglImage) {
        s_egl.eglDestroyImageKHR(m_display, m_eglImage);
    }

    if (m_fbo) {
        s_gles2.glDeleteFramebuffers(1, &m_fbo);
    }

    GLuint tex[2] = {m_tex, m_blitTex};
    s_gles2.glDeleteTextures(2, tex);
}

void ColorBuffer::readPixels(int x,
                             int y,
                             int width,
                             int height,
                             GLenum p_format,
                             GLenum p_type,
                             void* pixels) {
    ScopedHelperContext context(m_helper);
    if (!context.isOk()) {
        return;
    }

    if (bindFbo(&m_fbo, m_tex)) {
        s_gles2.glReadPixels(x, y, width, height, p_format, p_type, pixels);
        unbindFbo();
    }
}

void ColorBuffer::subUpdate(int x,
                            int y,
                            int width,
                            int height,
                            GLenum p_format,
                            GLenum p_type,
                            void* pixels) {
    static std::atomic<uint64_t> updateOrdinal(0);
    const uint64_t ordinal = updateOrdinal.fetch_add(1) + 1;
    const bool sample = aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal);
    if (sample) {
        aeGraphicsDiagLog("CB_SUBUPDATE",
                          "n=%llu cb=%p region=(%d,%d %dx%d) format=%#x type=%#x",
                          static_cast<unsigned long long>(ordinal), this,
                          x, y, width, height, p_format, p_type);
        if (p_format == GL_RGBA && p_type == GL_UNSIGNED_BYTE && pixels) {
            aeGraphicsDiagPixels("CB_SUBUPDATE_RGBA",
                                 static_cast<const unsigned char *>(pixels),
                                 width, height, static_cast<size_t>(width) * 4, false);
        }
    }
    ScopedHelperContext context(m_helper);
    if (!context.isOk()) {
        return;
    }

    GLint previousTexture = 0;
    GLint previousUnpack = 4;

    s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    s_gles2.glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpack);

    s_gles2.glBindTexture(GL_TEXTURE_2D, m_tex);
    s_gles2.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    s_gles2.glTexSubImage2D(
            GL_TEXTURE_2D, 0, x, y, width, height, p_format, p_type, pixels);
#ifdef AE_SYNC_SHARED_IMAGES
    if (!aeGraphicsDiagEnabled("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH")) {
        s_gles2.glFinish();
    }
#endif

    s_gles2.glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpack);
    s_gles2.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
}

bool ColorBuffer::blitFromCurrentReadBuffer()
{
    RenderThreadInfo *tInfo = RenderThreadInfo::get();
    if (!tInfo || !tInfo->currContext.Ptr()) {
        return false;
    }

    static std::atomic<uint64_t> reverseOrdinal(0);
    const uint64_t ordinal = reverseOrdinal.fetch_add(1) + 1;
    const bool sample = aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal);
    const bool probe = aeGraphicsDiagPixelFingerprints() && aeGraphicsDiagSample(ordinal);
    bool useCpuReverseBlit = aeGraphicsDiagEnabled("AE_DIAG_CPU_REVERSE_BLIT");
#ifdef AE_FORCE_CPU_COLORBUFFER_BLIT
    useCpuReverseBlit = true;
#endif
    if (sample) {
        aeGraphicsDiagLog("REVERSE_BEGIN",
                          "n=%llu cb=%p size=%ux%u gl2=%d path=%s",
                          static_cast<unsigned long long>(ordinal), this,
                          m_width, m_height, tInfo->currContext->isGL2(),
                          useCpuReverseBlit ? "cpu" : "eglimage");
    }
    if (useCpuReverseBlit) {
    // Diagnostic path: bypass only the reverse EGLImage handoff.
    // The forward m_tex -> m_eglImage path remains enabled for gralloc/WebView.
    const size_t pixelCount =
            static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
    std::vector<unsigned char> rgba(pixelCount * 4);

    if (tInfo->currContext->isGL2()) {
        GLint previousFbo = 0;
        GLint previousPack = 4;
        s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
        s_gles2.glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        s_gles2.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        s_gles2.glReadPixels(0, 0, m_width, m_height,
                             GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        s_gles2.glFinish();
        s_gles2.glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, previousFbo);
    } else {
        GLint previousFbo = 0;
        GLint previousPack = 4;
        s_gles1.glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &previousFbo);
        s_gles1.glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
        s_gles1.glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
        s_gles1.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        s_gles1.glReadPixels(0, 0, m_width, m_height,
                             GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        s_gles1.glFinish();
        s_gles1.glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
        s_gles1.glBindFramebufferOES(GL_FRAMEBUFFER_OES, previousFbo);
    }

    if (probe) {
        aeGraphicsDiagPixels("REV_CPU_GUEST_FBO", rgba.data(),
                             m_width, m_height,
                             static_cast<size_t>(m_width) * 4, false);
    }

    const GLenum uploadFormat =
            m_internalFormat == GL_RGB ? GL_RGB : GL_RGBA;
    const size_t components = uploadFormat == GL_RGB ? 3u : 4u;
    const size_t uploadRowBytes = static_cast<size_t>(m_width) * components;
    std::vector<unsigned char> upload(
            static_cast<size_t>(m_height) * uploadRowBytes);

    // The normal TextureDraw blit flips GL bottom-left rows into gralloc's
    // top-left convention. Do the same conversion explicitly here.
    for (GLuint y = 0; y < m_height; ++y) {
        const unsigned char *src =
                rgba.data() +
                static_cast<size_t>(m_height - 1u - y) *
                        static_cast<size_t>(m_width) * 4u;
        unsigned char *dst =
                upload.data() + static_cast<size_t>(y) * uploadRowBytes;

        if (uploadFormat == GL_RGBA) {
            memcpy(dst, src, static_cast<size_t>(m_width) * 4u);
        } else {
            for (GLuint x = 0; x < m_width; ++x) {
                dst[x * 3u + 0u] = src[x * 4u + 0u];
                dst[x * 3u + 1u] = src[x * 4u + 1u];
                dst[x * 3u + 2u] = src[x * 4u + 2u];
            }
        }
    }

    ScopedHelperContext context(m_helper);
    if (!context.isOk()) {
        return false;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint previousUnpack = 4;
    s_gles2.glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    s_gles2.glActiveTexture(GL_TEXTURE0);
    s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    s_gles2.glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpack);

    s_gles2.glBindTexture(GL_TEXTURE_2D, m_tex);
    s_gles2.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    s_gles2.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height,
                            uploadFormat, GL_UNSIGNED_BYTE, upload.data());
    s_gles2.glFinish();

    if (probe && uploadFormat == GL_RGBA) {
        std::vector<unsigned char> actual;
        GLenum status = 0;
        aeGraphicsDiagPixels("REV_CPU_UPLOAD_RGBA", upload.data(),
                             m_width, m_height,
                             static_cast<size_t>(m_width) * 4, false);
        if (diagnosticReadTextureRGBA(m_tex, m_width, m_height, &actual, &status)) {
            aeGraphicsDiagPixels("REV_CPU_MTEX_RGBA", actual.data(),
                                 m_width, m_height,
                                 static_cast<size_t>(m_width) * 4, false);
            aeGraphicsDiagComparePixels(
                    "REV_CPU_UPLOAD_COMPARE",
                    upload.data(), static_cast<size_t>(m_width) * 4, false,
                    actual.data(), static_cast<size_t>(m_width) * 4, false,
                    m_width, m_height, false);
        } else {
            aeGraphicsDiagLog("REV_CPU_MTEX_RGBA",
                              "probe failed framebufferStatus=%#x", status);
        }
    }
    s_gles2.glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpack);
    s_gles2.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    s_gles2.glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (sample) aeGraphicsDiagLog("REVERSE_END", "n=%llu cb=%p ok=1 path=cpu",
                                  static_cast<unsigned long long>(ordinal), this);
    return true;
    } else {
    // Normal path: copy the guest window framebuffer into the reverse EGLImage.
    // Keep the temporary EGLImage target alive until the producer commands have
    // completed; this matters for the custom ANGLE/Metal EGLImage implementation.
    GLuint tmpTex = 0;
    GLint currTexBind = 0;
    GLint currFramebuffer = 0;
    std::vector<unsigned char> guestProbe;

    if (tInfo->currContext->isGL2()) {
        s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currFramebuffer);
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (probe) {
            GLint previousPack = 4;
            s_gles2.glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
            s_gles2.glPixelStorei(GL_PACK_ALIGNMENT, 1);
            guestProbe.resize(static_cast<size_t>(m_width) * m_height * 4);
            s_gles2.glReadPixels(0, 0, m_width, m_height,
                                 GL_RGBA, GL_UNSIGNED_BYTE, guestProbe.data());
            s_gles2.glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
            aeGraphicsDiagPixels("REV_GUEST_FBO", guestProbe.data(),
                                 m_width, m_height,
                                 static_cast<size_t>(m_width) * 4, false);
        }
        s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &currTexBind);
        s_gles2.glGenTextures(1, &tmpTex);
        s_gles2.glBindTexture(GL_TEXTURE_2D, tmpTex);
        s_gles2.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_blitEGLImage);
        s_gles2.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                    m_width, m_height);
#ifdef AE_SYNC_SHARED_IMAGES
        if (!aeGraphicsDiagEnabled("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH")) {
            s_gles2.glFinish();
        }
#endif
        s_gles2.glBindTexture(GL_TEXTURE_2D, currTexBind);
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, currFramebuffer);
    } else {
        s_gles1.glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &currFramebuffer);
        s_gles1.glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
        if (probe) {
            GLint previousPack = 4;
            s_gles1.glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
            s_gles1.glPixelStorei(GL_PACK_ALIGNMENT, 1);
            guestProbe.resize(static_cast<size_t>(m_width) * m_height * 4);
            s_gles1.glReadPixels(0, 0, m_width, m_height,
                                 GL_RGBA, GL_UNSIGNED_BYTE, guestProbe.data());
            s_gles1.glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
            aeGraphicsDiagPixels("REV_GUEST_FBO", guestProbe.data(),
                                 m_width, m_height,
                                 static_cast<size_t>(m_width) * 4, false);
        }
        s_gles1.glGetIntegerv(GL_TEXTURE_BINDING_2D, &currTexBind);
        s_gles1.glGenTextures(1, &tmpTex);
        s_gles1.glBindTexture(GL_TEXTURE_2D, tmpTex);
        s_gles1.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_blitEGLImage);
        s_gles1.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                    m_width, m_height);
#ifdef AE_SYNC_SHARED_IMAGES
        if (!aeGraphicsDiagEnabled("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH")) {
            s_gles1.glFinish();
        }
#endif
        s_gles1.glBindTexture(GL_TEXTURE_2D, currTexBind);
        s_gles1.glBindFramebufferOES(GL_FRAMEBUFFER_OES, currFramebuffer);
    }

    ScopedHelperContext context(m_helper);
    if (!context.isOk()) {
        if (tmpTex) {
            if (tInfo->currContext->isGL2())
                s_gles2.glDeleteTextures(1, &tmpTex);
            else
                s_gles1.glDeleteTextures(1, &tmpTex);
        }
        if (sample) aeGraphicsDiagLog("REVERSE_END", "n=%llu cb=%p ok=0 helperContext",
                                      static_cast<unsigned long long>(ordinal), this);
        return false;
    }

    std::vector<unsigned char> blitProbe;
    if (probe) {
        GLenum status = 0;
        if (diagnosticReadTextureRGBA(
                m_blitTex, m_width, m_height, &blitProbe, &status)) {
            aeGraphicsDiagPixels("REV_BLIT_TEX", blitProbe.data(),
                                 m_width, m_height,
                                 static_cast<size_t>(m_width) * 4, false);
            if (!guestProbe.empty()) {
                aeGraphicsDiagComparePixels(
                        "REV_GUEST_TO_BLIT",
                        guestProbe.data(), static_cast<size_t>(m_width) * 4, false,
                        blitProbe.data(), static_cast<size_t>(m_width) * 4, false,
                        m_width, m_height, false);
            }
        } else {
            aeGraphicsDiagLog("REV_BLIT_TEX",
                              "probe failed framebufferStatus=%#x", status);
        }
    }

    if (!bindFbo(&m_fbo, m_tex)) {
        context.release();
        if (tmpTex) {
            if (tInfo->currContext->isGL2())
                s_gles2.glDeleteTextures(1, &tmpTex);
            else
                s_gles1.glDeleteTextures(1, &tmpTex);
        }
        return false;
    }

    GLint vport[4] = { 0, };
    s_gles2.glGetIntegerv(GL_VIEWPORT, vport);
    s_gles2.glViewport(0, 0, m_width, m_height);

    bool drawn = m_helper->getTextureDraw()->draw(m_blitTex, 0.);
#ifdef AE_SYNC_SHARED_IMAGES
    if (!aeGraphicsDiagEnabled("AE_DIAG_DISABLE_SHARED_IMAGE_FINISH")) {
        s_gles2.glFinish();
    }
#endif

    if (probe && drawn) {
        std::vector<unsigned char> outputProbe;
        GLenum status = 0;
        if (diagnosticReadTextureRGBA(
                m_tex, m_width, m_height, &outputProbe, &status)) {
            aeGraphicsDiagPixels("REV_MTEX", outputProbe.data(),
                                 m_width, m_height,
                                 static_cast<size_t>(m_width) * 4, false);
            if (!blitProbe.empty()) {
                aeGraphicsDiagComparePixels(
                        "REV_BLIT_TO_MTEX_FLIP",
                        blitProbe.data(), static_cast<size_t>(m_width) * 4, false,
                        outputProbe.data(), static_cast<size_t>(m_width) * 4, false,
                        m_width, m_height, true);
            }
        } else {
            aeGraphicsDiagLog("REV_MTEX",
                              "probe failed framebufferStatus=%#x", status);
        }
    }

    s_gles2.glViewport(vport[0], vport[1], vport[2], vport[3]);
    unbindFbo();

    // AndroidEmu sync/lifetime hardening v1:
    // delete reverse EGLImage target only after helper consumption.
    // ScopedHelperContext::release restores the original guest context first.
    context.release();
    if (tmpTex) {
        if (tInfo->currContext->isGL2())
            s_gles2.glDeleteTextures(1, &tmpTex);
        else
            s_gles1.glDeleteTextures(1, &tmpTex);
    }

    if (sample) aeGraphicsDiagLog("REVERSE_END", "n=%llu cb=%p ok=%d path=eglimage",
                                  static_cast<unsigned long long>(ordinal), this, drawn);
    return drawn;
    }
}

bool ColorBuffer::bindToTexture() {
    if (!m_eglImage) {
        return false;
    }
    RenderThreadInfo *tInfo = RenderThreadInfo::get();
    if (!tInfo->currContext.Ptr()) {
        return false;
    }

    static std::atomic<uint64_t> forwardOrdinal(0);
    const uint64_t ordinal = forwardOrdinal.fetch_add(1) + 1;
    const bool sample = aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal);
    const bool probe = aeGraphicsDiagPixelFingerprints() && aeGraphicsDiagSample(ordinal);
    std::vector<unsigned char> sourceProbe;

    if (sample) {
        GLint texture = 0;
        if (tInfo->currContext->isGL2())
            s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        else
            s_gles1.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        aeGraphicsDiagLog("FORWARD_BIND_BEGIN",
                          "n=%llu cb=%p image=%p guestTex=%d gl2=%d size=%ux%u",
                          static_cast<unsigned long long>(ordinal), this,
                          m_eglImage, texture, tInfo->currContext->isGL2(),
                          m_width, m_height);
    }

    if (probe) {
        sourceProbe.resize(static_cast<size_t>(m_width) * m_height * 4);
        if (readback(sourceProbe.data())) {
            aeGraphicsDiagPixels("FWD_SOURCE_MTEX", sourceProbe.data(),
                                 m_width, m_height,
                                 static_cast<size_t>(m_width) * 4, false);
        } else {
            sourceProbe.clear();
            aeGraphicsDiagLog("FWD_SOURCE_MTEX", "readback failed cb=%p", this);
        }
    }

    if (tInfo->currContext->isGL2()) {
        s_gles2.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
        if (probe) {
            GLint texture = 0;
            s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            std::vector<unsigned char> importedProbe;
            GLenum status = 0;
            if (diagnosticReadTextureRGBA(
                    static_cast<GLuint>(texture), m_width, m_height,
                    &importedProbe, &status)) {
                aeGraphicsDiagPixels("FWD_IMPORTED_TEX", importedProbe.data(),
                                     m_width, m_height,
                                     static_cast<size_t>(m_width) * 4, false);
                if (!sourceProbe.empty()) {
                    aeGraphicsDiagComparePixels(
                            "FWD_SOURCE_TO_IMPORTED",
                            sourceProbe.data(), static_cast<size_t>(m_width) * 4, false,
                            importedProbe.data(), static_cast<size_t>(m_width) * 4, false,
                            m_width, m_height, false);
                }
            } else {
                aeGraphicsDiagLog("FWD_IMPORTED_TEX",
                                  "probe failed guestTex=%d framebufferStatus=%#x",
                                  texture, status);
            }
        }
    }
    else {
        s_gles1.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
        if (probe) {
            aeGraphicsDiagLog("FWD_IMPORTED_TEX",
                              "pixel probe skipped for GLES1 context cb=%p", this);
        }
    }
    if (sample) {
        aeGraphicsDiagLog("FORWARD_BIND_END",
                          "n=%llu cb=%p", static_cast<unsigned long long>(ordinal), this);
    }
    return true;
}

bool ColorBuffer::bindToRenderbuffer() {
    if (!m_eglImage) {
        return false;
    }
    RenderThreadInfo *tInfo = RenderThreadInfo::get();
    if (!tInfo->currContext.Ptr()) {
        return false;
    }
    if (tInfo->currContext->isGL2()) {
        s_gles2.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER_OES, m_eglImage);
    }
    else {
        s_gles1.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER_OES, m_eglImage);
    }
    return true;
}

bool ColorBuffer::post(float rotation) {
    // NOTE: Do not call m_helper->setupContext() here!
    return m_helper->getTextureDraw()->draw(m_tex, rotation);
}

bool ColorBuffer::readback(unsigned char* img) {
    ScopedHelperContext context(m_helper);
    if (!context.isOk()) {
        return false;
    }
    if (bindFbo(&m_fbo, m_tex)) {
        s_gles2.glGetError(); // Separate readback status from earlier helper work.
        s_gles2.glReadPixels(
                0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, img);
        bool valid = s_gles2.glGetError() == GL_NO_ERROR;
        unbindFbo();
        return valid;
    }
    return false;
}
