#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "4f53dfeef92d2da880257e2620195531966e04d1"
ROOT = Path.cwd()

FILES = {
    "cb_h": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/ColorBuffer.h",
    "cb_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/ColorBuffer.cpp",
    "ws_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp",
}

EXPECTED_BLOBS = {
    "cb_h": "97e18461d8534ee13eda11d93a9894d2daca7807",
    "cb_cpp": "5264cda4e1cd2337b2a80eedf7cf4e6fadda5b0b",
    "ws_cpp": "26aa89c67dbbc568f038037ce6073688ba9dc536",
}

MARKER = "AndroidEmu shared-context reverse GPU v6"

def die(msg: str) -> None:
    print("ERROR: " + msg, file=sys.stderr)
    raise SystemExit(1)

def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()

try:
    head = git("rev-parse", "HEAD")
except Exception as exc:
    die(f"not a git repository: {exc}")

print("HEAD=" + head)
if head != EXPECTED_HEAD:
    die(
        "wrong HEAD. This modifier was prepared only for "
        f"{EXPECTED_HEAD}, but current HEAD is {head}."
    )

for key, path in FILES.items():
    if not path.is_file():
        die(f"missing source file: {path}")
    actual = git("hash-object", str(path))
    expected = EXPECTED_BLOBS[key]
    if actual != expected:
        die(
            f"{path}: working-tree blob mismatch; expected {expected}, "
            f"got {actual}. No files changed."
        )

original = {key: path.read_text(encoding="utf-8") for key, path in FILES.items()}
if any(MARKER in text for text in original.values()):
    print("OK: V6 shared-context reverse GPU path already appears to be applied.")
    raise SystemExit(0)

edited = dict(original)

def replace_exact(key: str, old: str, new: str, description: str) -> None:
    count = edited[key].count(old)
    if count != 1:
        die(f"{description}: expected exactly 1 match, found {count}; no files changed")
    edited[key] = edited[key].replace(old, new, 1)
    print("PLAN: " + description)

replace_exact(
    "cb_h",
    '''    // Copy the content of the current context's read surface to this
    // ColorBuffer. This is used from WindowSurface::flushColorBuffer().
    // Return true on success, false on failure (e.g. no current context).
    bool blitFromCurrentReadBuffer();

    // Read the content of the whole ColorBuffer as 32-bit RGBA pixels.
''',
    '''    // Copy the content of the current context's read surface to this
    // ColorBuffer. This is used from WindowSurface::flushColorBuffer().
    // Return true on success, false on failure (e.g. no current context).
    bool blitFromCurrentReadBuffer();

    // AndroidEmu shared-context reverse GPU v6.
    // The current context must be GLES2, current on the WindowSurface PBuffer,
    // and in the helper share group that owns m_tex/m_blitTex/TextureDraw.
    // This performs the reverse copy entirely on the GPU without EGLImage.
    bool blitFromCurrentReadBufferSharedGPU();

    // Read the content of the whole ColorBuffer as 32-bit RGBA pixels.
''',
    "ColorBuffer.h: declare direct shared-context GPU reverse copy",
)

shared_gpu_impl = r'''
// AndroidEmu shared-context reverse GPU v6
//
// Reverse EGLImage is unnecessary when WindowSurface can temporarily make its
// dedicated ES2 restore context current. That context already shares GL objects
// with ColorBuffer's helper group, so copy the PBuffer directly into m_blitTex
// and use the existing TextureDraw flip into m_tex.
//
// This deliberately uses a temporary FBO because framebuffer objects are
// context-local even though the attached textures are shared.
bool ColorBuffer::blitFromCurrentReadBufferSharedGPU()
{
    static std::atomic<uint64_t> sharedGpuOrdinal(0);
    const uint64_t ordinal = sharedGpuOrdinal.fetch_add(1) + 1;
    const bool sample =
            aeGraphicsDiagTraceEnabled() && aeGraphicsDiagSample(ordinal);

    if (!m_width || !m_height || !m_tex || !m_blitTex) {
        if (sample) {
            aeGraphicsDiagLog("REVERSE_SHARED_GPU_END",
                              "n=%llu cb=%p ok=0 invalid-storage",
                              static_cast<unsigned long long>(ordinal), this);
        }
        return false;
    }

    GLint previousFbo = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture0 = 0;

    s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    s_gles2.glGetIntegerv(GL_VIEWPORT, previousViewport);
    s_gles2.glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    s_gles2.glActiveTexture(GL_TEXTURE0);
    s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);

    if (sample) {
        aeGraphicsDiagLog("REVERSE_SHARED_GPU_BEGIN",
                          "n=%llu cb=%p size=%ux%u tex=%u blitTex=%u",
                          static_cast<unsigned long long>(ordinal), this,
                          m_width, m_height, m_tex, m_blitTex);
    }

    // Read from the WindowSurface PBuffer's default framebuffer.
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_gles2.glBindTexture(GL_TEXTURE_2D, m_blitTex);

    // Separate this operation's status from any earlier dedicated-context work.
    s_gles2.glGetError();
    s_gles2.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                m_width, m_height);
    const GLenum copyError = s_gles2.glGetError();

    GLuint localFbo = 0;
    GLenum framebufferStatus = 0;
    bool drawn = false;
    GLenum drawError = GL_NO_ERROR;

    if (copyError == GL_NO_ERROR) {
        s_gles2.glGenFramebuffers(1, &localFbo);
        s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, localFbo);
        s_gles2.glFramebufferTexture2D(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0_OES,
                GL_TEXTURE_2D, m_tex, 0);
        framebufferStatus = s_gles2.glCheckFramebufferStatus(GL_FRAMEBUFFER);

        if (framebufferStatus == GL_FRAMEBUFFER_COMPLETE_OES) {
            s_gles2.glViewport(0, 0, m_width, m_height);
            s_gles2.glDisable(GL_SCISSOR_TEST);
            s_gles2.glDisable(GL_BLEND);
            s_gles2.glDisable(GL_DEPTH_TEST);
            s_gles2.glDisable(GL_STENCIL_TEST);
            s_gles2.glDisable(GL_CULL_FACE);
            s_gles2.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            // TextureDraw uses vertically flipped texture coordinates, matching
            // the established reverse EGLImage path and the known-good CPU path.
            drawn = m_helper->getTextureDraw()->draw(m_blitTex, 0.0f);
            s_gles2.glFinish();
            drawError = s_gles2.glGetError();
        }
    }

    // Restore only bindings/state needed by the dedicated shared context.
    s_gles2.glBindFramebuffer(
            GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    if (localFbo) {
        s_gles2.glDeleteFramebuffers(1, &localFbo);
    }
    s_gles2.glViewport(previousViewport[0], previousViewport[1],
                       previousViewport[2], previousViewport[3]);
    s_gles2.glActiveTexture(GL_TEXTURE0);
    s_gles2.glBindTexture(
            GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
    s_gles2.glActiveTexture(
            static_cast<GLenum>(previousActiveTexture));

    const bool ok =
            copyError == GL_NO_ERROR &&
            framebufferStatus == GL_FRAMEBUFFER_COMPLETE_OES &&
            drawn &&
            drawError == GL_NO_ERROR;

    if (sample) {
        aeGraphicsDiagLog("REVERSE_SHARED_GPU_END",
                          "n=%llu cb=%p ok=%d copyError=%#x fboStatus=%#x "
                          "drawn=%d drawError=%#x",
                          static_cast<unsigned long long>(ordinal), this, ok,
                          copyError, framebufferStatus, drawn, drawError);
    }
    return ok;
}

'''

anchor = "\nbool ColorBuffer::bindToTexture() {\n"
count = edited["cb_cpp"].count(anchor)
if count != 1:
    die(
        "ColorBuffer.cpp: shared GPU insertion anchor expected exactly 1 "
        f"match, found {count}"
    )
edited["cb_cpp"] = edited["cb_cpp"].replace(
    anchor, "\n" + shared_gpu_impl + "bool ColorBuffer::bindToTexture() {\n", 1)
print("PLAN: ColorBuffer.cpp: add reverse GPU copy that bypasses EGLImage")

old_flush = '''    bool copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();
    // AndroidEmu previous-completed-frame restore v5
    mAttachedColorBufferFlushed = copied;

    // restore current context/surface
    bool restored = s_egl.eglMakeCurrent(mDisplay, prevDrawSurf, prevReadSurf, prevContext);
'''

new_flush = '''    // AndroidEmu shared-context reverse GPU v6
    //
    // CPU remains a diagnostic/reference path. Normal GPU operation switches
    // this same PBuffer to the dedicated ES2 context that shares ColorBuffer
    // textures with FrameBuffer's helper group, avoiding reverse EGLImage
    // aliasing entirely.
    bool useCpuReverseBlit =
            aeGraphicsDiagEnabled("AE_DIAG_CPU_REVERSE_BLIT");
#ifdef AE_FORCE_CPU_COLORBUFFER_BLIT
    useCpuReverseBlit = true;
#endif
    const bool useLegacyReverseEglImage =
            aeGraphicsDiagEnabled("AE_DIAG_LEGACY_REVERSE_EGLIMAGE");

    bool copied = false;
    const char *reversePath = "legacy-eglimage";

    if (!useCpuReverseBlit &&
        !useLegacyReverseEglImage &&
        mRestoreContext != EGL_NO_CONTEXT) {
        reversePath = "shared-gpu";

        // Finish the guest render pass before rebinding the same PBuffer to a
        // different context. This also forces Metal to store its attachment.
        if (mDrawContext->isGL2()) {
            s_gles2.glFinish();
        } else {
            s_gles1.glFinish();
        }

        if (s_egl.eglMakeCurrent(
                    mDisplay, mSurface, mSurface, mRestoreContext)) {
            copied =
                    mAttachedColorBuffer->blitFromCurrentReadBufferSharedGPU();
        } else {
            fprintf(stderr,
                    "Renderer error: failed to bind shared reverse GPU context "
                    "eglError=%#x\\n",
                    s_egl.eglGetError());
        }
    } else {
        if (useCpuReverseBlit) {
            reversePath = "cpu";
        }
        copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();
    }

    if (sample) {
        aeGraphicsDiagLog("WIN_REVERSE_PATH",
                          "n=%llu win=%p cb=%p path=%s copied=%d",
                          static_cast<unsigned long long>(ordinal),
                          this, mAttachedColorBuffer.Ptr(),
                          reversePath, copied);
    }

    // AndroidEmu previous-completed-frame restore v5
    mAttachedColorBufferFlushed = copied;

    // restore current context/surface
    bool restored = s_egl.eglMakeCurrent(
            mDisplay, prevDrawSurf, prevReadSurf, prevContext);
'''

replace_exact(
    "ws_cpp",
    old_flush,
    new_flush,
    "WindowSurface.cpp: make shared-context GPU copy the normal reverse path",
)

required = {
    "cb_h": [
        MARKER,
        "blitFromCurrentReadBufferSharedGPU();",
    ],
    "cb_cpp": [
        MARKER,
        "ColorBuffer::blitFromCurrentReadBufferSharedGPU()",
        "REVERSE_SHARED_GPU_BEGIN",
        "glCopyTexSubImage2D",
        "m_helper->getTextureDraw()->draw(m_blitTex, 0.0f)",
    ],
    "ws_cpp": [
        MARKER,
        "AE_DIAG_LEGACY_REVERSE_EGLIMAGE",
        "blitFromCurrentReadBufferSharedGPU()",
        'reversePath = "shared-gpu"',
    ],
}

for key, tokens in required.items():
    for token in tokens:
        if token not in edited[key]:
            die(f"post-edit validation failed: {FILES[key]} missing {token!r}")

changed = [key for key in FILES if edited[key] != original[key]]
if set(changed) != set(FILES):
    die(f"unexpected changed-file set: {changed}")

for key in changed:
    FILES[key].write_text(edited[key], encoding="utf-8")
    print("WROTE: " + str(FILES[key].relative_to(ROOT)))

try:
    subprocess.run(["git", "diff", "--check"], check=True)
except subprocess.CalledProcessError:
    for key in changed:
        FILES[key].write_text(original[key], encoding="utf-8")
    die("git diff --check failed; all V6 edits were rolled back")

print()
print("OK: V6 shared-context reverse GPU path applied.")
print("Normal test:")
print("  AE_DIAG_CPU_REVERSE_BLIT: unset/OFF")
print("  AE_DIAG_LEGACY_REVERSE_EGLIMAGE: unset/OFF")
print()
print("A/B controls:")
print("  AE_DIAG_CPU_REVERSE_BLIT=1          -> known-good CPU path")
print("  AE_DIAG_LEGACY_REVERSE_EGLIMAGE=1  -> old noisy GPU EGLImage path")
print()
print("Review:")
print("  git diff --check")
print("  git diff --stat")
