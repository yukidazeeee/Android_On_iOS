"""Pinned ANGLE Metal additions for Goldfish's GL texture EGLImages.

Keep selected mip storage shared with the source. EGLImage owns its exported
Metal TextureRef, so source texture orphaning must not mutate the old storage.
Only advertise the texture-2D source target implemented here.
"""
BASE = 'Source/ThirdParty/ANGLE/src/libANGLE/renderer/metal/'
REPLACEMENTS = [
    (BASE + 'ImageMtl.mm',
     '#include "libANGLE/renderer/metal/ImageMtl.h"',
     '#include "libANGLE/renderer/metal/ImageMtl.h"\n\n#include <cstdlib>'),
    (BASE + 'DisplayMtl.mm',
     '    outExtensions->imageBase = true;',
     '    outExtensions->imageBase = true;\n    outExtensions->glTexture2DImage = true;'),
    (BASE + 'TextureMtl.h',
     '    const mtl::TextureRef &getNativeTexture() const { return mNativeTexture; }',
     '''    const mtl::TextureRef &getNativeTexture() const { return mNativeTexture; }

    // Share the selected mip's native storage, not a snapshot of its pixels.
    // The returned TextureRef is a strong reference owned by ImageMtl.
    angle::Result exportEGLImage(const gl::Context *context,
                                 const gl::ImageIndex &index,
                                 mtl::TextureRef *out)
    {
        if (!out)
            return angle::Result::Stop;
        if (ensureTextureCreated(context) != angle::Result::Continue ||
            ensureImageCreated(context, index) != angle::Result::Continue)
            return angle::Result::Stop;
        mtl::TextureRef image = getImage(index);
        if (!image)
            return angle::Result::Stop;
        *out = image;
        return angle::Result::Continue;
    }'''),
    (BASE + 'ImageMtl.h',
     '    gl::TextureType mImageTextureType;',
     '''    // Borrowed only between creation and synchronous initialize().
    const gl::Context *mSourceContext;
    gl::TextureType mImageTextureType;'''),
    (BASE + 'ImageMtl.mm',
     'ImageMtl::ImageMtl(const egl::ImageState &state, const gl::Context *context) : ImageImpl(state) {}',
     '''ImageMtl::ImageMtl(const egl::ImageState &state, const gl::Context *context)
    : ImageImpl(state), mSourceContext(context) {}'''),
    (BASE + 'ImageMtl.mm',
     '''    if (mState.target == EGL_METAL_TEXTURE_ANGLE)
    {''',
     '''    if (mState.target == EGL_GL_TEXTURE_2D_KHR)
    {
        TextureMtl *texture = GetImplAs<TextureMtl>(GetAs<gl::Texture>(mState.source));
        if (!mSourceContext ||
            texture->exportEGLImage(mSourceContext, mState.imageIndex, &mNativeTexture) !=
                angle::Result::Continue)
        {
            mSourceContext = nullptr;
            return egl::EglBadAlloc();
        }
        mSourceContext = nullptr;
        // exportEGLImage returns a single-mip view, so the imported level is 0.
        mImageTextureType = gl::TextureType::_2D;
        mImageLevel = 0;
        mImageLayer = 0;
    }
    else if (mState.target == EGL_METAL_TEXTURE_ANGLE)
    {'''),
    (BASE + 'ImageMtl.mm',
     '''    if (sibling == mState.source)
    {
        mNativeTexture = nullptr;
    }''',
     '''    // Runtime diagnostic: normally GL texture EGLImages keep their exported
    // TextureRef across source orphaning. Optionally drop it to test whether
    // stale exported Metal storage is involved in a rendering artifact.
    (void)context;
    if (sibling == mState.source)
    {
        if (mState.target == EGL_GL_TEXTURE_2D_KHR)
        {
            if (std::getenv("AE_DIAG_DROP_EGLIMAGE_ON_ORPHAN"))
                mNativeTexture = nullptr;
        }
        else
        {
            mNativeTexture = nullptr;
        }
    }'''),

    # AndroidEmu: force Metal color attachment preservation.
    #
    # Goldfish's guest EGL window surface is represented by one persistent host
    # PBuffer while Android BufferQueue color buffers rotate behind it. Android
    # UI/SF frequently performs partial redraw and blending, both of which need
    # the previous destination pixels to remain defined. Metal's DontCare
    # store/load actions make those pixels undefined and can surface as stale
    # rectangular tiles. Preserve color attachments unconditionally; depth and
    # stencil keep ANGLE's normal discard behavior.
    (BASE + 'FrameBufferMtl.mm',
     '''            colorAttachment.storeAction = MTLStoreActionDontCare;
            if (renderPassStarted)
            {
                encoder->setColorStoreAction(MTLStoreActionDontCare, i);
            }''',
     '''            // AndroidEmu: force Metal color attachment preservation.
            // Ignore color invalidation: the Goldfish host PBuffer is the
            // persistent backing for partial redraw across guest swaps.
            colorAttachment.storeAction = MTLStoreActionStore;
            if (renderPassStarted)
            {
                encoder->setColorStoreAction(MTLStoreActionStore, i);
            }'''),

    (BASE + 'FrameBufferMtl.mm',
     '''    // Compute loadOp based on previous storeOp and reset storeOp flags:
    for (mtl::RenderPassColorAttachmentDesc &colorAttachment : mRenderPassDesc.colorAttachments)
    {
        forceDepthStencilMultisampleLoad |=
            colorAttachment.storeAction == MTLStoreActionStoreAndMultisampleResolve;
        setLoadStoreActionOnRenderPassFirstStart(&colorAttachment, false);
    }''',
     '''    // AndroidEmu: force Metal color attachment preservation.
    // Never start a color render pass with DontCare. Transparent blending and
    // partial redraw both consume destination pixels from the previous pass.
    for (mtl::RenderPassColorAttachmentDesc &colorAttachment : mRenderPassDesc.colorAttachments)
    {
        forceDepthStencilMultisampleLoad |=
            colorAttachment.storeAction == MTLStoreActionStoreAndMultisampleResolve;
        colorAttachment.loadAction = MTLLoadActionLoad;
        if (colorAttachment.hasImplicitMSTexture())
        {
            colorAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        }
        else
        {
            colorAttachment.storeAction = MTLStoreActionStore;
        }
    }'''),
]
