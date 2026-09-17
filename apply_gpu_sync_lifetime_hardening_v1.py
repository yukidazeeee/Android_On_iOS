#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys

EXPECTED_HEAD = "555e643213255b51f25aba785484e20149af6928"

ROOT = Path.cwd()

FILES = {
    "fb_h": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.h",
    "fb_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/FrameBuffer.cpp",
    "rc_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/RenderControl.cpp",
    "cb_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/ColorBuffer.cpp",
    "ws_cpp": ROOT / "ThirdParty/EmuGL/upstream/host/libs/libOpenglRender/WindowSurface.cpp",
    "display_c": ROOT / "ThirdParty/AndroidQemuCompat/qemu/display.c",
}

MARKER = "AndroidEmu sync/lifetime hardening v1"

def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)

def git_head() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except Exception:
        return ""

for key, path in FILES.items():
    if not path.is_file():
        die(f"missing {path}")

head = git_head()
print(f"HEAD={head or '(unknown)'}")
if head and head != EXPECTED_HEAD:
    print(
        f"WARNING: script was prepared against {EXPECTED_HEAD}; "
        f"current HEAD is {head}. Pattern validation will still protect the files.",
        file=sys.stderr,
    )

original = {key: path.read_text(encoding="utf-8") for key, path in FILES.items()}
edited = dict(original)

def replace_once(key: str, old: str, new: str, desc: str) -> None:
    text = edited[key]
    if new in text:
        print(f"SKIP: {desc} (already applied)")
        return
    count = text.count(old)
    if count != 1:
        die(f"{desc}: expected exactly 1 source match, found {count}")
    edited[key] = text.replace(old, new, 1)
    print(f"PLAN: {desc}")

# 1) Goldfish cache/post synchronization.
replace_once(
    "fb_h",
    "#include <map>\n\n#include <stdint.h>\n",
    "#include <map>\n#include <condition_variable>\n#include <mutex>\n\n#include <stdint.h>\n",
    "FrameBuffer.h: add post synchronization primitives",
)

replace_once(
    "fb_h",
    """    bool  bindColorBufferToRenderbuffer(HandleType p_colorbuffer);
    void  readColorBuffer(HandleType p_colorbuffer,
""",
    """    bool  bindColorBufferToRenderbuffer(HandleType p_colorbuffer);
    int   colorBufferCacheFlush(HandleType p_colorbuffer,
                                EGLint postCount,
                                int forRead);
    void  readColorBuffer(HandleType p_colorbuffer,
""",
    "FrameBuffer.h: expose colorBufferCacheFlush",
)

replace_once(
    "fb_h",
    """    ColorBufferMap m_colorbuffers;
    ColorBuffer::Helper* m_colorBufferHelper;
""",
    """    ColorBufferMap m_colorbuffers;
    // AndroidEmu sync/lifetime hardening v1
    // Guest gralloc increments a per-buffer post counter before rcFBPost and
    // later passes that value to rcColorBufferCacheFlush. Track completed
    // host posts independently of m_lock so a cache-flush thread can wait
    // without preventing the post thread from entering FrameBuffer::post().
    std::mutex m_colorBufferPostMutex;
    std::condition_variable m_colorBufferPostCv;
    std::map<HandleType, uint32_t> m_colorBufferCompletedPosts;
    ColorBuffer::Helper* m_colorBufferHelper;
""",
    "FrameBuffer.h: add per-ColorBuffer completed-post state",
)

replace_once(
    "fb_cpp",
    "#include <stdio.h>\n#include <atomic>\n",
    "#include <stdio.h>\n#include <atomic>\n#include <chrono>\n",
    "FrameBuffer.cpp: add bounded-wait clock support",
)

create_pat = re.compile(
    r"""(?P<indent>[ \t]*)m_colorbuffers\[ret\]\.cb = cb;\n"""
    r"""(?P=indent)m_colorbuffers\[ret\]\.refcount = 1;\n"""
)
if "m_colorBufferCompletedPosts[ret] = 0;" not in edited["fb_cpp"]:
    ms = list(create_pat.finditer(edited["fb_cpp"]))
    if len(ms) != 1:
        die(f"FrameBuffer.cpp: createColorBuffer state init: expected 1 match, found {len(ms)}")
    m = ms[0]
    indent = m.group("indent")
    replacement = (
        f"{indent}m_colorbuffers[ret].cb = cb;\n"
        f"{indent}m_colorbuffers[ret].refcount = 1;\n"
        f"{indent}{{\n"
        f"{indent}    std::lock_guard<std::mutex> syncLock(m_colorBufferPostMutex);\n"
        f"{indent}    m_colorBufferCompletedPosts[ret] = 0;\n"
        f"{indent}}}\n"
    )
    edited["fb_cpp"] = edited["fb_cpp"][:m.start()] + replacement + edited["fb_cpp"][m.end():]
    print("PLAN: FrameBuffer.cpp: initialize completed-post counter")
else:
    print("SKIP: FrameBuffer.cpp: initialize completed-post counter (already applied)")

post_exit_old = """EXIT:
    if (needLock) {
        m_lock.unlock();
    }
    return ret;
}
"""
post_exit_new = """EXIT:
    if (ret) {
        {
            std::lock_guard<std::mutex> syncLock(m_colorBufferPostMutex);
            std::map<HandleType, uint32_t>::iterator it =
                    m_colorBufferCompletedPosts.find(p_colorbuffer);
            if (it != m_colorBufferCompletedPosts.end()) {
                ++it->second;
            }
        }
        m_colorBufferPostCv.notify_all();
    }
    if (needLock) {
        m_lock.unlock();
    }
    return ret;
}
"""
replace_once(
    "fb_cpp", post_exit_old, post_exit_new,
    "FrameBuffer.cpp: count completed rcFBPost operations"
)

if "m_colorBufferCompletedPosts.erase(p_colorbuffer);" not in edited["fb_cpp"]:
    close_rx = re.compile(
        r"""(void FrameBuffer::closeColorBuffer\(HandleType p_colorbuffer\)\s*\{.*?"""
        r"""if \(--\(\*c\)\.second\.refcount == 0\) \{\s*)"""
        r"""m_colorbuffers\.erase\(c\);"""
        r"""(\s*\}.*?\n\})""",
        re.S,
    )
    matches = list(close_rx.finditer(edited["fb_cpp"]))
    if len(matches) != 1:
        die(f"FrameBuffer.cpp: closeColorBuffer cleanup: expected 1 match, found {len(matches)}")
    def close_repl(m):
        return (
            m.group(1)
            + "m_colorbuffers.erase(c);\n"
              "        {\n"
              "            std::lock_guard<std::mutex> syncLock(m_colorBufferPostMutex);\n"
              "            m_colorBufferCompletedPosts.erase(p_colorbuffer);\n"
              "        }\n"
              "        m_colorBufferPostCv.notify_all();"
            + m.group(2)
        )
    edited["fb_cpp"] = close_rx.sub(close_repl, edited["fb_cpp"], count=1)
    print("PLAN: FrameBuffer.cpp: remove completed-post state on ColorBuffer destruction")
else:
    print("SKIP: FrameBuffer.cpp: remove completed-post state (already applied)")

if "FrameBuffer::colorBufferCacheFlush" not in edited["fb_cpp"]:
    needle = "bool FrameBuffer::bindColorBufferToTexture(HandleType p_colorbuffer)"
    if edited["fb_cpp"].count(needle) != 1:
        die("FrameBuffer.cpp: cannot locate bindColorBufferToTexture insertion point")
    impl = r"""// AndroidEmu sync/lifetime hardening v1
int FrameBuffer::colorBufferCacheFlush(HandleType p_colorbuffer,
                                       EGLint postCount,
                                       int forRead)
{
    if (postCount < 0) {
        return -1;
    }

    // Goldfish gralloc flushes the connection that issued rcFBPost, but host
    // RenderThreads are independent. Wait for the matching processed post
    // without holding FrameBuffer::m_lock, otherwise the post cannot finish.
    {
        std::unique_lock<std::mutex> syncLock(m_colorBufferPostMutex);
        const std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            std::map<HandleType, uint32_t>::const_iterator it =
                    m_colorBufferCompletedPosts.find(p_colorbuffer);
            if (it == m_colorBufferCompletedPosts.end()) {
                return -1;
            }
            if (it->second >= static_cast<uint32_t>(postCount)) {
                break;
            }
            if (m_colorBufferPostCv.wait_until(syncLock, deadline) ==
                std::cv_status::timeout) {
                fprintf(stderr,
                        "EmuGL: rcColorBufferCacheFlush timeout cb=%#x "
                        "wantedPosts=%d completedPosts=%u forRead=%d\n",
                        p_colorbuffer, postCount, it->second, forRead);
                return -1;
            }
        }
    }

    // Finish the shared helper context after the post barrier. This makes the
    // host-side consumer complete shared-resource work before guest gralloc
    // reuses or CPU-reads the buffer.
    emugl::Mutex::AutoLock mutex(m_lock);
    ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
    if (c == m_colorbuffers.end()) {
        return -1;
    }

    ScopedBind bind(this);
    if (!bind.isValid()) {
        return -1;
    }
    s_gles2.glFinish();

    // Conservative legacy behavior: a positive value tells gralloc to do a
    // CPU readback. Extra readback is safe; falsely returning 0 can be stale.
    return forRead ? 1 : 0;
}

"""
    edited["fb_cpp"] = edited["fb_cpp"].replace(needle, impl + needle, 1)
    print("PLAN: FrameBuffer.cpp: implement Goldfish post-count/cache barrier")
else:
    print("SKIP: FrameBuffer.cpp: colorBufferCacheFlush (already applied)")

replace_once(
    "rc_cpp",
    """static EGLint rcColorBufferCacheFlush(uint32_t colorBuffer,
                                      EGLint postCount, int forRead)
{
   // XXX: TBD - should be implemented
   return 0;
}
""",
    """static EGLint rcColorBufferCacheFlush(uint32_t colorBuffer,
                                      EGLint postCount, int forRead)
{
    FrameBuffer *fb = FrameBuffer::getFB();
    if (!fb) {
        return -1;
    }
    return fb->colorBufferCacheFlush(colorBuffer, postCount, forRead);
}
""",
    "RenderControl.cpp: replace rcColorBufferCacheFlush no-op",
)

# 2) Reverse EGLImage lifetime.
if "delete reverse EGLImage target only after helper consumption" not in edited["cb_cpp"]:
    text = edited["cb_cpp"]
    start = text.find("bool ColorBuffer::blitFromCurrentReadBuffer()")
    end = text.find("bool ColorBuffer::bindToTexture()", start)
    if start < 0 or end < 0:
        die("ColorBuffer.cpp: cannot locate blitFromCurrentReadBuffer")
    body = text[start:end]

    old_delete2 = "        s_gles2.glDeleteTextures(1, &tmpTex);\n"
    old_delete1 = "        s_gles1.glDeleteTextures(1, &tmpTex);\n"
    if body.count(old_delete2) != 1 or body.count(old_delete1) != 1:
        die(
            "ColorBuffer.cpp: expected exactly one GL2 and one GL1 early "
            "tmpTex delete in reverse path"
        )
    body = body.replace(old_delete2, "", 1).replace(old_delete1, "", 1)

    helper_fail_old = """    ScopedHelperContext context(m_helper);
    if (!context.isOk()) {
        if (sample) aeGraphicsDiagLog("REVERSE_END", "n=%llu cb=%p ok=0 helperContext",
                                      static_cast<unsigned long long>(ordinal), this);
        return false;
    }
"""
    helper_fail_new = """    ScopedHelperContext context(m_helper);
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
"""
    if body.count(helper_fail_old) != 1:
        die("ColorBuffer.cpp: helper-context failure block changed unexpectedly")
    body = body.replace(helper_fail_old, helper_fail_new, 1)

    bind_fail_old = """    if (!bindFbo(&m_fbo, m_tex)) {
        return false;
    }
"""
    bind_fail_new = """    if (!bindFbo(&m_fbo, m_tex)) {
        context.release();
        if (tmpTex) {
            if (tInfo->currContext->isGL2())
                s_gles2.glDeleteTextures(1, &tmpTex);
            else
                s_gles1.glDeleteTextures(1, &tmpTex);
        }
        return false;
    }
"""
    if body.count(bind_fail_old) != 1:
        die("ColorBuffer.cpp: reverse bindFbo failure block changed unexpectedly")
    body = body.replace(bind_fail_old, bind_fail_new, 1)

    tail_old = """    s_gles2.glViewport(vport[0], vport[1], vport[2], vport[3]);
    unbindFbo();
    if (sample) aeGraphicsDiagLog("REVERSE_END", "n=%llu cb=%p ok=%d path=eglimage",
                                  static_cast<unsigned long long>(ordinal), this, drawn);
    return drawn;
"""
    tail_new = """    s_gles2.glViewport(vport[0], vport[1], vport[2], vport[3]);
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
"""
    if body.count(tail_old) != 1:
        die("ColorBuffer.cpp: reverse path tail changed unexpectedly")
    body = body.replace(tail_old, tail_new, 1)
    edited["cb_cpp"] = text[:start] + body + text[end:]
    print("PLAN: ColorBuffer.cpp: keep reverse EGLImage target alive through helper copy")
else:
    print("SKIP: ColorBuffer.cpp: reverse EGLImage lifetime (already applied)")

# 3) Existing attach-clear diagnostic must run after the pbuffer is current.
replace_once(
    "ws_cpp",
    """    if (!s_egl.eglMakeCurrent(mDisplay,
                              mSurface,
                              mSurface,
                              mDrawContext->getEGLContext())) {
        fprintf(stderr, "Error making draw context current\\n");
        return false;
    }

    bool copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();
""",
    """    if (!s_egl.eglMakeCurrent(mDisplay,
                              mSurface,
                              mSurface,
                              mDrawContext->getEGLContext())) {
        fprintf(stderr, "Error making draw context current\\n");
        return false;
    }

    // The attach-time call can occur while another surface is current.
    // Run the pending diagnostic poison now that this pbuffer is current.
    applyDiagnosticAttachClear();

    bool copied = mAttachedColorBuffer->blitFromCurrentReadBuffer();
""",
    "WindowSurface.cpp: execute pending attach clear after eglMakeCurrent",
)

# 4) Stable tight QEMU-side GPU shadow + correct DisplaySurface stride.
replace_once(
    "display_c",
    """    bool valid, blank, invalidate, base_pending, presented, gpu_presented;
    uint8_t *scanout;
} GFDisplay;
""",
    """    bool valid, blank, invalidate, base_pending, presented, gpu_presented;
    uint8_t *scanout;
    uint8_t *gpu_scanout; /* AndroidEmu sync/lifetime hardening v1 */
} GFDisplay;
""",
    "display.c: add persistent tight GPU shadow",
)

replace_once(
    "display_c",
    """        unsigned first, rows;
        if (ae_gpu_frame_region(surface_data(surface), (size_t)GF_WIDTH * GF_HEIGHT * 4, &first, &rows)) {
            s->gpu_presented = true;
            dpy_gfx_update(s->console, 0, first, GF_WIDTH, rows);
            android51_host_frame(surface_data(surface), surface_stride(surface), 0, first, GF_WIDTH, rows);
        }
""",
    """        unsigned first, rows;
        const size_t tight_stride = (size_t)GF_WIDTH * 4;
        const size_t tight_size = tight_stride * GF_HEIGHT;
        if (ae_gpu_frame_region(s->gpu_scanout, tight_size, &first, &rows)) {
            for (unsigned y = first; y < first + rows; ++y) {
                memcpy(surface_data(surface) + (size_t)y * surface_stride(surface),
                       s->gpu_scanout + (size_t)y * tight_stride,
                       tight_stride);
            }
            s->gpu_presented = true;
            dpy_gfx_update(s->console, 0, first, GF_WIDTH, rows);
            android51_host_frame(surface_data(surface), surface_stride(surface), 0, first, GF_WIDTH, rows);
        }
""",
    "display.c: make GPU frame copy respect QEMU DisplaySurface stride",
)

replace_once(
    "display_c",
    """    s->valid = s->blank = s->base_pending = s->presented = false;
    s->invalidate = true;
    display_irq(s);
""",
    """    s->valid = s->blank = s->base_pending = s->presented = false;
    s->gpu_presented = false;
    s->invalidate = true;
    if (s->board->gpu_ready) { ae_gpu_invalidate_frame(); }
    display_irq(s);
""",
    "display.c: clear GPU presentation state on reset",
)

replace_once(
    "display_c",
    """    s->scanout = g_malloc0((size_t)GF_WIDTH * GF_HEIGHT * 2);
    s->console = graphic_console_init(NULL, 0, &graphic_ops, s);
""",
    """    s->scanout = g_malloc0((size_t)GF_WIDTH * GF_HEIGHT * 2);
    s->gpu_scanout = g_malloc0((size_t)GF_WIDTH * GF_HEIGHT * 4);
    s->console = graphic_console_init(NULL, 0, &graphic_ops, s);
""",
    "display.c: allocate persistent tight GPU shadow",
)

changed = [key for key in FILES if edited[key] != original[key]]
if not changed:
    print("No changes needed: all fixes appear to be applied already.")
    sys.exit(0)

checks = [
    ("fb_h", "colorBufferCacheFlush(", 1),
    ("fb_cpp", "FrameBuffer::colorBufferCacheFlush(", 1),
    ("rc_cpp", "fb->colorBufferCacheFlush(", 1),
    ("cb_cpp", "delete reverse EGLImage target only after helper consumption", 1),
    ("ws_cpp", "Run the pending diagnostic poison now that this pbuffer is current.", 1),
    ("display_c", "uint8_t *gpu_scanout;", 1),
]
for key, token, minimum in checks:
    if edited[key].count(token) < minimum:
        die(f"post-edit validation failed for {FILES[key]}: missing {token!r}")

# All source matching succeeded. Only now write files.
for key in changed:
    FILES[key].write_text(edited[key], encoding="utf-8")
    print(f"WROTE: {FILES[key].relative_to(ROOT)}")

try:
    subprocess.run(["git", "diff", "--check"], check=True)
except subprocess.CalledProcessError:
    die("git diff --check failed; inspect/revert the generated diff before building")

print("\nApplied fixes:")
print("  1. Goldfish rcColorBufferCacheFlush post-count barrier (bounded 2 s)")
print("  2. Reverse EGLImage temporary texture lifetime extended through consumer copy")
print("  3. Existing pbuffer attach-clear diagnostic made effective")
print("  4. QEMU GPU scanout copied through a tight persistent shadow using real surface stride")
print("  5. GPU presentation state invalidated on display reset")
print("\nReview:")
print("  git diff --check")
print("  git diff --stat")
print("  git diff -- ThirdParty/EmuGL ThirdParty/AndroidQemuCompat/qemu/display.c")
