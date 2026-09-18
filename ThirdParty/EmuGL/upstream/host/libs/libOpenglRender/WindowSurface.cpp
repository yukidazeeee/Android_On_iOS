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
#include "WindowSurface.h"

#include "FbConfig.h"
#include "EGLDispatch.h"
#include "GraphicsDiagnostics.h"
#include "GLErrorLog.h"
#include "GLESv1Dispatch.h"
#include "GLESv2Dispatch.h"

#include <GLES2/gl2.h>
#include <GLES/glext.h>

#include <stdio.h>
#include <string.h>
#include <atomic>


WindowSurface::WindowSurface(EGLDisplay display,
                             EGLConfig config) :
        mSurface(NULL),
        mAttachedColorBuffer(NULL),
        mReadContext(NULL),
        mDrawContext(NULL),
        mWidth(0),
        mHeight(0),
        mConfig(config),
        mDisplay(display),
        mRestoreContext(EGL_NO_CONTEXT),
        mDiagnosticClearPending(false) {}

WindowSurface::~WindowSurface() {
    if (mRestoreContext != EGL_NO_CONTEXT) {
        s_egl.eglDestroyContext(mDisplay, mRestoreContext);
        mRestoreContext = EGL_NO_CONTEXT;
    }
    if (mSurface != EGL_NO_SURFACE) {
        s_egl.eglDestroySurface(mDisplay, mSurface);
        mSurface = EGL_NO_SURFACE;
    }
}

WindowSurface *WindowSurface::create(EGLDisplay display,
                                     EGLConfig config,
                                     int p_width,
                                     int p_height,
                                     EGLContext restoreShareContext) {
    // allocate space for the WindowSurface object
    WindowSurface *win = new WindowSurface(display, config);
    if (!win) {
        return NULL;
    }

    // Create a pbuffer to be used as the egl surface
    // for that window.
    if (!win->resize(p_width, p_height)) {
        delete win;
        return NULL;
    }

    // AndroidEmu ColorBuffer -> PBuffer restore v3
    // The normal guest RenderContext is not necessarily in the same GL share
    // group as ColorBuffer::m_tex. Create an ES2 context using the exact
    // WindowSurface EGLConfig, but share it with FrameBuffer's helper group.
    // This lets us draw the target ColorBuffer into this PBuffer reliably.
    if (restoreShareContext != EGL_NO_CONTEXT) {
        const EGLint restoreAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        win->mRestoreContext = s_egl.eglCreateContext(
                display, config, restoreShareContext, restoreAttribs);
        if (win->mRestoreContext == EGL_NO_CONTEXT) {
            fprintf(stderr,
                    "Renderer error: failed to create ColorBuffer restore "
                    "context eglError=%#x\n",
                    s_egl.eglGetError());
            delete win;
            return NULL;
        }
    }

    return win;
}


// AndroidEmu ColorBuffer -> PBuffer restore v3
bool WindowSurface::restoreColorBuffer() {
    if (!mAttachedColorBuffer.Ptr()) {
        return true;
    }

    // GLES1-only configs cannot share/use TextureDraw's ES2 objects. They keep
    // the legacy path; Android 5/6 SurfaceFlinger/HWUI normally use ES2.
    if (mRestoreContext == EGL_NO_CONTEXT) {
        if (aeGraphicsDiagTraceEnabled()) {
            aeGraphicsDiagLog("WIN_RESTORE_CB",
                              "win=%p cb=%p skipped=no-es2-restore-context",
                              this, mAttachedColorBuffer.Ptr());
        }
        return true;
    }

    if (!mWidth || !mHeight ||
        mAttachedColorBuffer->getWidth() != mWidth ||
        mAttachedColorBuffer->getHeight() != mHeight) {
        fprintf(stderr, "Renderer error: restore ColorBuffer dimensions mismatch\n");
        return false;
    }

    const EGLContext prevContext = s_egl.eglGetCurrentContext();
    const EGLSurface prevReadSurf = s_egl.eglGetCurrentSurface(EGL_READ);
    const EGLSurface prevDrawSurf = s_egl.eglGetCurrentSurface(EGL_DRAW);

    if (!s_egl.eglMakeCurrent(
                mDisplay, mSurface, mSurface, mRestoreContext)) {
        fprintf(stderr,
                "Renderer error: failed to bind ColorBuffer restore context "
                "eglError=%#x\n",
                s_egl.eglGetError());
        return false;
    }

    GLint previousFbo = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    s_gles2.glGetIntegerv(GL_VIEWPORT, previousViewport);

    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_gles2.glViewport(0, 0, mWidth, mHeight);
    s_gles2.glDisable(GL_SCISSOR_TEST);
    s_gles2.glDisable(GL_BLEND);
    s_gles2.glDisable(GL_DEPTH_TEST);
    s_gles2.glDisable(GL_STENCIL_TEST);
    s_gles2.glDisable(GL_CULL_FACE);
    s_gles2.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (mDiagnosticClearPending) {
        GLfloat previousClearColor[4] = {0, 0, 0, 0};
        s_gles2.glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
        s_gles2.glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        s_gles2.glClear(GL_COLOR_BUFFER_BIT);
        s_gles2.glClearColor(previousClearColor[0],
                             previousClearColor[1],
                             previousClearColor[2],
                             previousClearColor[3]);
        mDiagnosticClearPending = false;
    }

    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("WIN_RESTORE_CB_BEGIN",
                          "win=%p pbuffer=%p cb=%p size=%ux%u ctx=%p",
                          this, mSurface, mAttachedColorBuffer.Ptr(),
                          mWidth, mHeight, mRestoreContext);
    }

    s_gles2.glGetError();
    const bool drawn = mAttachedColorBuffer->post(0.0f);
    s_gles2.glFinish();
    const GLenum restoreError = s_gles2.glGetError();

    s_gles2.glBindFramebuffer(
            GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    s_gles2.glViewport(previousViewport[0], previousViewport[1],
                       previousViewport[2], previousViewport[3]);

    const bool contextRestored = s_egl.eglMakeCurrent(
            mDisplay, prevDrawSurf, prevReadSurf, prevContext);

    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("WIN_RESTORE_CB_END",
                          "win=%p cb=%p drawn=%d glError=%#x contextRestored=%d",
                          this, mAttachedColorBuffer.Ptr(),
                          drawn, restoreError, contextRestored);
    }

    return drawn && restoreError == GL_NO_ERROR && contextRestored;
}


void WindowSurface::setColorBuffer(ColorBufferPtr p_colorBuffer) {
    ColorBuffer *previous = mAttachedColorBuffer.Ptr();
    mAttachedColorBuffer = p_colorBuffer;
    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("WIN_ATTACH_CB",
                          "win=%p pbuffer=%p oldCb=%p newCb=%p size=%ux%u",
                          this, mSurface, previous, mAttachedColorBuffer.Ptr(),
                          mWidth, mHeight);
    }

    // resize the window if the attached color buffer is of different
    // size.
    unsigned int cbWidth = mAttachedColorBuffer->getWidth();
    unsigned int cbHeight = mAttachedColorBuffer->getHeight();

    if (cbWidth != mWidth || cbHeight != mHeight) {
        resize(cbWidth, cbHeight);
    }
    mDiagnosticClearPending =
            aeGraphicsDiagEnabled("AE_DIAG_CLEAR_PBUFFER_ON_ATTACH");
    applyDiagnosticAttachClear();
}

void WindowSurface::applyDiagnosticAttachClear() {
    if (!mDiagnosticClearPending || !mDrawContext.Ptr() ||
        s_egl.eglGetCurrentSurface(EGL_DRAW) != mSurface) {
        return;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    GLfloat clearColor[4] = {0, 0, 0, 0};

    if (mDrawContext->isGL2()) {
        s_gles2.glGetIntegerv(GL_VIEWPORT, viewport);
        s_gles2.glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        s_gles2.glViewport(0, 0, mWidth, mHeight);
        s_gles2.glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        s_gles2.glClear(GL_COLOR_BUFFER_BIT);
        s_gles2.glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        s_gles2.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    } else {
        s_gles1.glGetIntegerv(GL_VIEWPORT, viewport);
        s_gles1.glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        s_gles1.glViewport(0, 0, mWidth, mHeight);
        s_gles1.glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        s_gles1.glClear(GL_COLOR_BUFFER_BIT);
        s_gles1.glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        s_gles1.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    mDiagnosticClearPending = false;
}

void WindowSurface::bind(RenderContextPtr p_ctx, BindType p_bindType) {
    if (p_bindType == BIND_READ) {
        mReadContext = p_ctx;
    } else if (p_bindType == BIND_DRAW) {
        mDrawContext = p_ctx;
    } else if (p_bindType == BIND_READDRAW) {
        mReadContext = p_ctx;
        mDrawContext = p_ctx;
    }
}

bool WindowSurface::flushColorBuffer() {
    static std::atomic<uint64_t> flushOrdinal(0);
    const uint64_t ordinal = flushOrdinal.fetch_add(1) + 1;
    const bool sample = aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal);
    if (sample) {
        aeGraphicsDiagLog("WIN_FLUSH_BEGIN",
                          "n=%llu win=%p pbuffer=%p cb=%p size=%ux%u drawCtx=%p currentCtx=%p",
                          static_cast<unsigned long long>(ordinal), this, mSurface,
                          mAttachedColorBuffer.Ptr(), mWidth, mHeight,
                          mDrawContext.Ptr() ? mDrawContext->getEGLContext() : EGL_NO_CONTEXT,
                          s_egl.eglGetCurrentContext());
    }
    if (!mAttachedColorBuffer.Ptr()) {
        if (sample) aeGraphicsDiagLog("WIN_FLUSH_END",
                                     "n=%llu noColorBuffer",
                                     static_cast<unsigned long long>(ordinal));
        return true;
    }
    if (!mWidth || !mHeight) {
        return false;
    }

    if (mAttachedColorBuffer->getWidth() != mWidth ||
        mAttachedColorBuffer->getHeight() != mHeight) {
        // XXX: should never happen - how this needs to be handled?
        fprintf(stderr, "Dimensions do not match\n");
        return false;
    }

    if (!mDrawContext.Ptr()) {
        fprintf(stderr, "Draw context is NULL\n");
        return false;
    }

    // Make the surface current
    EGLContext prevContext = s_egl.eglGetCurrentContext();
    EGLSurface prevReadSurf = s_egl.eglGetCurrentSurface(EGL_READ);
    EGLSurface prevDrawSurf = s_egl.eglGetCurrentSurface(EGL_DRAW);

    if (!s_egl.eglMakeCurrent(mDisplay,
                              mSurface,
                              mSurface,
                              mDrawContext->getEGLContext())) {
        fprintf(stderr, "Error making draw context current\n");
        return false;
    }

    // The attach-time call can occur while another surface is current.
    // Run the pending diagnostic poison now that this pbuffer is current.
    applyDiagnosticAttachClear();

    bool copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();

    // restore current context/surface
    bool restored = s_egl.eglMakeCurrent(mDisplay, prevDrawSurf, prevReadSurf, prevContext);
    if (sample) {
        aeGraphicsDiagLog("WIN_FLUSH_END",
                          "n=%llu copied=%d restored=%d cb=%p",
                          static_cast<unsigned long long>(ordinal),
                          copied, restored, mAttachedColorBuffer.Ptr());
    }
    return copied && restored;
}

bool WindowSurface::resize(unsigned int p_width, unsigned int p_height)
{
    if (mSurface && mWidth == p_width && mHeight == p_height) {
        // no need to resize
        return true;
    }

    EGLContext prevContext = s_egl.eglGetCurrentContext();
    EGLSurface prevReadSurf = s_egl.eglGetCurrentSurface(EGL_READ);
    EGLSurface prevDrawSurf = s_egl.eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prevPbuf = mSurface;
    bool needRebindContext = mSurface &&
                             (prevReadSurf == mSurface ||
                              prevDrawSurf == mSurface);

    if (needRebindContext) {
        s_egl.eglMakeCurrent(
                mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    //
    // Destroy previous surface
    //
    if (mSurface) {
        s_egl.eglDestroySurface(mDisplay, mSurface);
        mSurface = NULL;
    }

    //
    // Create pbuffer surface.
    //
    const EGLint pbufAttribs[5] = {
        EGL_WIDTH, (EGLint) p_width, EGL_HEIGHT, (EGLint) p_height, EGL_NONE,
    };

    mSurface = s_egl.eglCreatePbufferSurface(mDisplay,
                                             mConfig,
                                             pbufAttribs);
    if (mSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "Renderer error: failed to create/resize pbuffer!!\n");
        return false;
    }

    mWidth = p_width;
    mHeight = p_height;

    if (needRebindContext) {
        s_egl.eglMakeCurrent(
                mDisplay,
                (prevDrawSurf == prevPbuf) ? mSurface : prevDrawSurf,
                (prevReadSurf == prevPbuf) ? mSurface : prevReadSurf,
                prevContext);
    }

    return true;
}
