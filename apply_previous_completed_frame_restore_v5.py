#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "3f50eb549ce918795ef8a9d80a03c0883c5ad65a"
ROOT = Path.cwd()

FILES = {
    "ws_h": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.h",
    "ws_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp",
    "fb_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp",
}

EXPECTED_BLOBS = {
    "ws_h": "b49dabd4af915305d41a06ce046e25e0b52b25fb",
    "ws_cpp": "c181fdf52f44e92055668a1bd9292446b12eb061",
    "fb_cpp": "f86fabac0d41493c4d50082e91ed61095504e0eb",
}

MARKER = "AndroidEmu previous-completed-frame restore v5"

def die(msg: str) -> None:
    print("ERROR: " + msg, file=sys.stderr)
    raise SystemExit(1)

def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()

head = git("rev-parse", "HEAD")
print("HEAD=" + head)
if head != EXPECTED_HEAD:
    die(f"wrong HEAD: expected {EXPECTED_HEAD}, got {head}")

for key, path in FILES.items():
    if not path.is_file():
        die(f"missing source file: {path}")
    actual_blob = git("hash-object", str(path))
    if actual_blob != EXPECTED_BLOBS[key]:
        die(
            f"{path}: working-tree blob mismatch; expected "
            f"{EXPECTED_BLOBS[key]}, got {actual_blob}"
        )

original = {k: p.read_text(encoding="utf-8") for k, p in FILES.items()}
if any(MARKER in text for text in original.values()):
    print("OK: V5 already applied.")
    raise SystemExit(0)

edited = dict(original)

def replace_exact(key: str, old: str, new: str, desc: str) -> None:
    count = edited[key].count(old)
    if count != 1:
        die(f"{desc}: expected exactly 1 match, found {count}; no files changed")
    edited[key] = edited[key].replace(old, new, 1)
    print("PLAN: " + desc)

replace_exact(
    "ws_h",
    """    // AndroidEmu ColorBuffer -> PBuffer restore v3
    // Restore the newly attached gralloc ColorBuffer into this window's host
    // PBuffer before the guest performs a partial redraw.
    bool restoreColorBuffer();
""",
    """    // AndroidEmu ColorBuffer -> PBuffer restore v3/v5.
    // Draw an explicitly selected ColorBuffer into this window's PBuffer.
    // V5 normally passes the immediately previous completed frame, not the
    // newly dequeued BufferQueue slot.
    bool restoreColorBuffer(ColorBufferPtr p_colorBuffer);
""",
    "WindowSurface.h: make restore source explicit",
)

replace_exact(
    "ws_h",
    """    EGLSurface mSurface;
    ColorBufferPtr mAttachedColorBuffer;
    RenderContextPtr mReadContext;
""",
    """    EGLSurface mSurface;
    ColorBufferPtr mAttachedColorBuffer;
    // AndroidEmu previous-completed-frame restore v5
    bool mAttachedColorBufferFlushed;
    RenderContextPtr mReadContext;
""",
    "WindowSurface.h: track completed frame",
)

replace_exact(
    "ws_cpp",
    """        mSurface(NULL),
        mAttachedColorBuffer(NULL),
        mReadContext(NULL),
""",
    """        mSurface(NULL),
        mAttachedColorBuffer(NULL),
        mAttachedColorBufferFlushed(false),
        mReadContext(NULL),
""",
    "WindowSurface.cpp: initialize completed-frame flag",
)

start = edited["ws_cpp"].find("// AndroidEmu ColorBuffer -> PBuffer restore v3\nbool WindowSurface::restoreColorBuffer() {")
end_marker = "\n\nvoid WindowSurface::setColorBuffer(ColorBufferPtr p_colorBuffer) {"
end = edited["ws_cpp"].find(end_marker)
if start < 0 or end < 0 or end <= start:
    die("WindowSurface.cpp: could not isolate existing restoreColorBuffer block")

old_restore = edited["ws_cpp"][start:end]

new_restore = """// AndroidEmu previous-completed-frame restore v5
bool WindowSurface::restoreColorBuffer(ColorBufferPtr p_colorBuffer) {
    if (!p_colorBuffer.Ptr()) {
        return true;
    }

    if (mRestoreContext == EGL_NO_CONTEXT) {
        if (aeGraphicsDiagTraceEnabled()) {
            aeGraphicsDiagLog("WIN_RESTORE_CB",
                              "win=%p cb=%p skipped=no-es2-restore-context",
                              this, p_colorBuffer.Ptr());
        }
        return true;
    }

    if (!mWidth || !mHeight ||
        p_colorBuffer->getWidth() != mWidth ||
        p_colorBuffer->getHeight() != mHeight) {
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
                          this, mSurface, p_colorBuffer.Ptr(),
                          mWidth, mHeight, mRestoreContext);
    }

    s_gles2.glGetError();
    const bool drawn = p_colorBuffer->post(0.0f);
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
                          this, p_colorBuffer.Ptr(),
                          drawn, restoreError, contextRestored);
    }

    return drawn && restoreError == GL_NO_ERROR && contextRestored;
}
"""
edited["ws_cpp"] = edited["ws_cpp"][:start] + new_restore + edited["ws_cpp"][end:]
print("PLAN: WindowSurface.cpp: make restore use explicit source ColorBuffer")

old_set = """void WindowSurface::setColorBuffer(ColorBufferPtr p_colorBuffer) {
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
"""

new_set = """void WindowSurface::setColorBuffer(ColorBufferPtr p_colorBuffer) {
    // AndroidEmu previous-completed-frame restore v5
    // Use the ColorBuffer that flushColorBuffer() just filled from the
    // immediately previous completed frame. Restoring the newly dequeued
    // BufferQueue slot instead can rewind untouched regions by several frames.
    ColorBufferPtr previous = mAttachedColorBuffer;
    const bool previousCompleted = mAttachedColorBufferFlushed;

    const unsigned int cbWidth = p_colorBuffer->getWidth();
    const unsigned int cbHeight = p_colorBuffer->getHeight();

    bool sizeReady = true;
    if (cbWidth != mWidth || cbHeight != mHeight) {
        sizeReady = resize(cbWidth, cbHeight);
    }

    bool restoredPrevious = true;
    if (sizeReady && previousCompleted && previous.Ptr() &&
        previous->getWidth() == mWidth &&
        previous->getHeight() == mHeight) {
        restoredPrevious = restoreColorBuffer(previous);
    }

    mAttachedColorBuffer = p_colorBuffer;
    mAttachedColorBufferFlushed = false;

    if (aeGraphicsDiagTraceEnabled()) {
        aeGraphicsDiagLog("WIN_ATTACH_CB",
                          "win=%p pbuffer=%p oldCb=%p newCb=%p size=%ux%u "
                          "previousCompleted=%d restoredPrevious=%d",
                          this, mSurface, previous.Ptr(),
                          mAttachedColorBuffer.Ptr(), mWidth, mHeight,
                          previousCompleted, restoredPrevious);
    }

    mDiagnosticClearPending =
            aeGraphicsDiagEnabled("AE_DIAG_CLEAR_PBUFFER_ON_ATTACH");
    applyDiagnosticAttachClear();
}
"""

replace_exact(
    "ws_cpp",
    old_set,
    new_set,
    "WindowSurface.cpp: seed PBuffer from immediately previous completed frame",
)

replace_exact(
    "ws_cpp",
    """    bool copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();

    // restore current context/surface
""",
    """    bool copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();
    // AndroidEmu previous-completed-frame restore v5
    mAttachedColorBufferFlushed = copied;

    // restore current context/surface
""",
    "WindowSurface.cpp: mark successful flush as completed frame",
)

replace_exact(
    "fb_cpp",
    """        const bool restored = (*w).second->restoreColorBuffer();
""",
    """        const bool restored =
                (*w).second->restoreColorBuffer((*c).second.cb);
""",
    "FrameBuffer.cpp: retain old dequeued-slot restore only for explicit A/B diagnostic",
)

required = {
    "ws_h": [
        "AndroidEmu previous-completed-frame restore v5",
        "restoreColorBuffer(ColorBufferPtr p_colorBuffer)",
        "mAttachedColorBufferFlushed",
    ],
    "ws_cpp": [
        "bool WindowSurface::restoreColorBuffer(ColorBufferPtr p_colorBuffer)",
        "const bool previousCompleted = mAttachedColorBufferFlushed;",
        "restoredPrevious = restoreColorBuffer(previous);",
        "mAttachedColorBufferFlushed = copied;",
    ],
    "fb_cpp": [
        "restoreColorBuffer((*c).second.cb)",
        "AE_DIAG_RESTORE_DEQUEUED_COLORBUFFER",
    ],
}

for key, tokens in required.items():
    for token in tokens:
        if token not in edited[key]:
            die(f"post-edit validation failed: {FILES[key]} missing {token!r}")

for key, path in FILES.items():
    path.write_text(edited[key], encoding="utf-8")

try:
    subprocess.run(["git", "diff", "--check"], check=True)
except subprocess.CalledProcessError:
    for key, path in FILES.items():
        path.write_text(original[key], encoding="utf-8")
    die("git diff --check failed; all V5 edits rolled back")

print()
print("OK: V5 previous-completed-frame restore applied.")
print("Test with:")
print("  reverse EGLImage -> CPU copy: ON")
print("  AE_DIAG_RESTORE_DEQUEUED_COLORBUFFER: unset/OFF")
