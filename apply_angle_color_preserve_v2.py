#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

EXPECTED_HEAD = "c2e8946a2b12f3281d635ea1aebc2c672603a4e8"
PATH = Path("scripts/angle_metal_image.py")
MARKER = "AndroidEmu: force Metal color attachment preservation"

def fail(msg):
    print("ERROR: " + msg, file=sys.stderr)
    raise SystemExit(1)

if not PATH.is_file():
    fail("run from repository root; missing " + str(PATH))

try:
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
except Exception:
    head = ""

print("HEAD=" + (head or "(unknown)"))
if head and head != EXPECTED_HEAD:
    print(
        "WARNING: prepared for " + EXPECTED_HEAD + "; current HEAD is " + head +
        ". Exact source-pattern validation is still enforced.",
        file=sys.stderr,
    )

src = PATH.read_text(encoding="utf-8")

if MARKER in src:
    print("OK: ANGLE color-preservation V2 is already present.")
    raise SystemExit(0)

needle = '    (BASE + \'ImageMtl.mm\',\n     \'\'\'    if (sibling == mState.source)\n    {\n        mNativeTexture = nullptr;\n    }\'\'\',\n     \'\'\'    // Runtime diagnostic: normally GL texture EGLImages keep their exported\n    // TextureRef across source orphaning. Optionally drop it to test whether\n    // stale exported Metal storage is involved in a rendering artifact.\n    (void)context;\n    if (sibling == mState.source)\n    {\n        if (mState.target == EGL_GL_TEXTURE_2D_KHR)\n        {\n            if (std::getenv("AE_DIAG_DROP_EGLIMAGE_ON_ORPHAN"))\n                mNativeTexture = nullptr;\n        }\n        else\n        {\n            mNativeTexture = nullptr;\n        }\n    }\'\'\'),\n]\n'
replacement = '    (BASE + \'ImageMtl.mm\',\n     \'\'\'    if (sibling == mState.source)\n    {\n        mNativeTexture = nullptr;\n    }\'\'\',\n     \'\'\'    // Runtime diagnostic: normally GL texture EGLImages keep their exported\n    // TextureRef across source orphaning. Optionally drop it to test whether\n    // stale exported Metal storage is involved in a rendering artifact.\n    (void)context;\n    if (sibling == mState.source)\n    {\n        if (mState.target == EGL_GL_TEXTURE_2D_KHR)\n        {\n            if (std::getenv("AE_DIAG_DROP_EGLIMAGE_ON_ORPHAN"))\n                mNativeTexture = nullptr;\n        }\n        else\n        {\n            mNativeTexture = nullptr;\n        }\n    }\'\'\'),\n\n    # AndroidEmu: force Metal color attachment preservation.\n    #\n    # Goldfish\'s guest EGL window surface is represented by one persistent host\n    # PBuffer while Android BufferQueue color buffers rotate behind it. Android\n    # UI/SF frequently performs partial redraw and blending, both of which need\n    # the previous destination pixels to remain defined. Metal\'s DontCare\n    # store/load actions make those pixels undefined and can surface as stale\n    # rectangular tiles. Preserve color attachments unconditionally; depth and\n    # stencil keep ANGLE\'s normal discard behavior.\n    (BASE + \'FrameBufferMtl.mm\',\n     \'\'\'            colorAttachment.storeAction = MTLStoreActionDontCare;\n            if (renderPassStarted)\n            {\n                encoder->setColorStoreAction(MTLStoreActionDontCare, i);\n            }\'\'\',\n     \'\'\'            // AndroidEmu: force Metal color attachment preservation.\n            // Ignore color invalidation: the Goldfish host PBuffer is the\n            // persistent backing for partial redraw across guest swaps.\n            colorAttachment.storeAction = MTLStoreActionStore;\n            if (renderPassStarted)\n            {\n                encoder->setColorStoreAction(MTLStoreActionStore, i);\n            }\'\'\'),\n\n    (BASE + \'FrameBufferMtl.mm\',\n     \'\'\'    // Compute loadOp based on previous storeOp and reset storeOp flags:\n    for (mtl::RenderPassColorAttachmentDesc &colorAttachment : mRenderPassDesc.colorAttachments)\n    {\n        forceDepthStencilMultisampleLoad |=\n            colorAttachment.storeAction == MTLStoreActionStoreAndMultisampleResolve;\n        setLoadStoreActionOnRenderPassFirstStart(&colorAttachment, false);\n    }\'\'\',\n     \'\'\'    // AndroidEmu: force Metal color attachment preservation.\n    // Never start a color render pass with DontCare. Transparent blending and\n    // partial redraw both consume destination pixels from the previous pass.\n    for (mtl::RenderPassColorAttachmentDesc &colorAttachment : mRenderPassDesc.colorAttachments)\n    {\n        forceDepthStencilMultisampleLoad |=\n            colorAttachment.storeAction == MTLStoreActionStoreAndMultisampleResolve;\n        colorAttachment.loadAction = MTLLoadActionLoad;\n        if (colorAttachment.hasImplicitMSTexture())\n        {\n            colorAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;\n        }\n        else\n        {\n            colorAttachment.storeAction = MTLStoreActionStore;\n        }\n    }\'\'\'),\n]\n'

if src.count(needle) != 1:
    fail("angle_metal_image.py tail does not match expected current source; no file was changed")

new_src = src.replace(needle, replacement, 1)

# Parse the generated replacement table before touching the repository.
compile(new_src, str(PATH), "exec")

PATH.write_text(new_src, encoding="utf-8")

try:
    subprocess.run(["git", "diff", "--check", "--", str(PATH)], check=True)
except subprocess.CalledProcessError:
    PATH.write_text(src, encoding="utf-8")
    fail("git diff --check failed; original file restored")

print("OK: patched scripts/angle_metal_image.py")
print("The iOS dependency key hashes angle_metal_image.py, so the ANGLE cache key changes.")
print("Review:")
print("  git diff --check")
print("  git diff -- scripts/angle_metal_image.py")
