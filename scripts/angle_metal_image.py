"""Pinned ANGLE Metal additions for Goldfish's GL texture EGLImages.

Keep selected mip storage shared with the source. EGLImage owns its exported
Metal TextureRef, so source texture orphaning must not mutate the old storage.
Only advertise the texture-2D source target implemented here.
"""
BASE = 'Source/ThirdParty/ANGLE/src/libANGLE/renderer/metal/'
REPLACEMENTS = [
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
     '''    // EGLImage siblings keep their exported TextureRef when the source GL
    // texture is deleted or respecified. Do not mutate TextureMtl here: the
    // old Metal storage remains alive until ImageMtl/onDestroy releases it.
    (void)context;
    if (sibling == mState.source && mState.target != EGL_GL_TEXTURE_2D_KHR)
    {
        mNativeTexture = nullptr;
    }'''),
]
