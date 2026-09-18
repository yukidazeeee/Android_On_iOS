#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "36d3520dfbab5a8c7459c9e0fef2447f8ee55fed"
ROOT = Path.cwd()

FILES = {
    "ws_h": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.h",
    "ws_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp",
    "fb_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp",
}

MARKER = "AndroidEmu ColorBuffer -> PBuffer restore v3"

def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)

def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()

try:
    head = git("rev-parse", "HEAD")
except Exception as exc:
    die(f"not a git repository: {exc}")

print(f"HEAD={head}")
if head != EXPECTED_HEAD:
    die(
        "wrong HEAD. This script is built only for "
        f"{EXPECTED_HEAD}, but current HEAD is {head}. "
        "Update/recreate the modifier instead of forcing it."
    )

for path in FILES.values():
    if not path.is_file():
        die(f"missing source file: {path}")

original = {key: path.read_text(encoding="utf-8") for key, path in FILES.items()}

if any(MARKER in text for text in original.values()):
    print("OK: V3 ColorBuffer -> PBuffer restore appears to be already applied.")
    raise SystemExit(0)

edited = dict(original)

def replace_exact(key: str, old: str, new: str, description: str) -> None:
    text = edited[key]
    count = text.count(old)
    if count != 1:
        die(f"{description}: expected exactly 1 source match, found {count}")
    edited[key] = text.replace(old, new, 1)
    print(f"PLAN: {description}")

replace_exact(
    "ws_h",
    """    static WindowSurface* create(EGLDisplay display,
                                 EGLConfig config,
                                 int width,
                                 int height);
""",
    """    static WindowSurface* create(EGLDisplay display,
                                 EGLConfig config,
                                 int width,
                                 int height,
                                 EGLContext restoreShareContext);
""",
    "WindowSurface.h: pass the helper share-group context into each window surface",
)

replace_exact(
    "ws_h",
    """    // Poison newly attached backing storage with magenta when requested.
    // Undrawn/preserved regions then become visually obvious.
    void applyDiagnosticAttachClear();

    // Copy the Pbuffer's pixels to the attached color buffer.
""",
    """    // Poison newly attached backing storage with magenta when requested.
    // Undrawn/preserved regions then become visually obvious.
    void applyDiagnosticAttachClear();

    // AndroidEmu ColorBuffer -> PBuffer restore v3
    // Restore the newly attached gralloc ColorBuffer into this window's host
    // PBuffer before the guest performs a partial redraw.
    bool restoreColorBuffer();

    // Copy the Pbuffer's pixels to the attached color buffer.
""",
    "WindowSurface.h: declare ColorBuffer -> PBuffer restore",
)

replace_exact(
    "ws_h",
    """    EGLConfig mConfig;
    EGLDisplay mDisplay;
    bool mDiagnosticClearPending;
""",
    """    EGLConfig mConfig;
    EGLDisplay mDisplay;
    // Dedicated ES2 context using this surface's exact EGLConfig. It shares
    // objects with FrameBuffer's helper context, so ColorBuffer::m_tex and
    // TextureDraw's program/buffers are valid here without depending on guest
    // context sharing.
    EGLContext mRestoreContext;
    bool mDiagnosticClearPending;
""",
    "WindowSurface.h: store the per-surface restore context",
)

replace_exact(
    "ws_cpp",
    """        mHeight(0),
        mConfig(config),
        mDisplay(display),
        mDiagnosticClearPending(false) {}
""",
    """        mHeight(0),
        mConfig(config),
        mDisplay(display),
        mRestoreContext(EGL_NO_CONTEXT),
        mDiagnosticClearPending(false) {}
""",
    "WindowSurface.cpp: initialize restore context",
)

replace_exact(
    "ws_cpp",
    """WindowSurface::~WindowSurface() {
    s_egl.eglDestroySurface(mDisplay, mSurface);
}
""",
    """WindowSurface::~WindowSurface() {
    if (mRestoreContext != EGL_NO_CONTEXT) {
        s_egl.eglDestroyContext(mDisplay, mRestoreContext);
        mRestoreContext = EGL_NO_CONTEXT;
    }
    if (mSurface != EGL_NO_SURFACE) {
        s_egl.eglDestroySurface(mDisplay, mSurface);
        mSurface = EGL_NO_SURFACE;
    }
}
""",
    "WindowSurface.cpp: destroy restore context safely",
)

replace_exact(
    "ws_cpp",
    """WindowSurface *WindowSurface::create(EGLDisplay display,
                                     EGLConfig config,
                                     int p_width,
                                     int p_height) {
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

    return win;
}
""",
    """WindowSurface *WindowSurface::create(EGLDisplay display,
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
                    "context eglError=%#x\\n",
                    s_egl.eglGetError());
            delete win;
            return NULL;
        }
    }

    return win;
}
""",
    "WindowSurface.cpp: create an ES2 restore context in the helper share group",
)

restore_impl = '''
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
        fprintf(stderr, "Renderer error: restore ColorBuffer dimensions mismatch\\n");
        return false;
    }

    const EGLContext prevContext = s_egl.eglGetCurrentContext();
    const EGLSurface prevReadSurf = s_egl.eglGetCurrentSurface(EGL_READ);
    const EGLSurface prevDrawSurf = s_egl.eglGetCurrentSurface(EGL_DRAW);

    if (!s_egl.eglMakeCurrent(
                mDisplay, mSurface, mSurface, mRestoreContext)) {
        fprintf(stderr,
                "Renderer error: failed to bind ColorBuffer restore context "
                "eglError=%#x\\n",
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

'''

anchor = "\nvoid WindowSurface::setColorBuffer(ColorBufferPtr p_colorBuffer) {\n"
anchor_count = edited["ws_cpp"].count(anchor)
if anchor_count != 1:
    die(
        "WindowSurface.cpp: restore insertion anchor expected exactly once, "
        f"found {anchor_count}"
    )
edited["ws_cpp"] = edited["ws_cpp"].replace(anchor, restore_impl + anchor, 1)
print("PLAN: WindowSurface.cpp: add full ColorBuffer -> PBuffer restore")

replace_exact(
    "fb_cpp",
    """    WindowSurfacePtr win(WindowSurface::create(
            getDisplay(), config->getEglConfig(), p_width, p_height));
""",
    """    // AndroidEmu ColorBuffer -> PBuffer restore v3
    // The restore renderer is GLES2 and must share ColorBuffer/TextureDraw GL
    // objects with the helper context. GLES1-only configs retain the legacy
    // behavior by passing EGL_NO_CONTEXT.
    const EGLContext restoreShareContext =
            (config->getRenderableType() & EGL_OPENGL_ES2_BIT)
                    ? m_pbufContext : EGL_NO_CONTEXT;
    WindowSurfacePtr win(WindowSurface::create(
            getDisplay(), config->getEglConfig(), p_width, p_height,
            restoreShareContext));
""",
    "FrameBuffer.cpp: create WindowSurface with helper share-group context",
)

replace_exact(
    "fb_cpp",
    """    (*w).second->setColorBuffer((*c).second.cb);
    return true;
}
""",
    """    (*w).second->setColorBuffer((*c).second.cb);

    // Restore this particular BufferQueue slot's previous pixels before the
    // guest starts its next partial redraw. Without this, one host PBuffer is
    // reused across A/B/C ColorBuffers and untouched regions inherit pixels
    // from the wrong slot (or the magenta diagnostic poison).
    const bool restored = (*w).second->restoreColorBuffer();
    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("FB_RESTORE_WINDOW_CB",
                          "surface=%#x colorbuffer=%#x restored=%d",
                          p_surface, p_colorbuffer, restored);
    }
    return restored;
}
""",
    "FrameBuffer.cpp: restore newly attached ColorBuffer before guest partial redraw",
)

required_tokens = {
    "ws_h": [
        "EGLContext restoreShareContext",
        "bool restoreColorBuffer();",
        "EGLContext mRestoreContext;",
    ],
    "ws_cpp": [
        "WindowSurface::restoreColorBuffer()",
        "WIN_RESTORE_CB_BEGIN",
        "WIN_RESTORE_CB_END",
        "mAttachedColorBuffer->post(0.0f)",
    ],
    "fb_cpp": [
        "const EGLContext restoreShareContext",
        "restoreColorBuffer();",
        "FB_RESTORE_WINDOW_CB",
    ],
}

for key, tokens in required_tokens.items():
    for token in tokens:
        if token not in edited[key]:
            die(f"post-edit validation failed: {FILES[key]} missing {token!r}")

changed = [key for key in FILES if edited[key] != original[key]]
if set(changed) != set(FILES):
    die(f"unexpected changed-file set: {changed}")

for key in changed:
    FILES[key].write_text(edited[key], encoding="utf-8")
    print(f"WROTE: {FILES[key].relative_to(ROOT)}")

try:
    subprocess.run(["git", "diff", "--check"], check=True)
except subprocess.CalledProcessError:
    for key in changed:
        FILES[key].write_text(original[key], encoding="utf-8")
    die("git diff --check failed; all V3 edits were rolled back")

print()
print("OK: V3 ColorBuffer -> PBuffer restore applied.")
print("Changed files:")
for key in changed:
    print("  " + str(FILES[key].relative_to(ROOT)))
print()
print("Review:")
print("  git diff --check")
print("  git diff --stat")
print("  git diff -- ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.h \\")
print("                 ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp \\")
print("                 ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp")
