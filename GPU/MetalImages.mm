#include "SharedImage.h"
#include "EGLDispatch.h"
#import <Metal/Metal.h>
#include <cstdint>

EGLImageKHR ae_gpu_create_native_image(EGLDisplay display, int width, int height, unsigned internalFormat) {
    @autoreleasepool {
        // Query ANGLE's actual device: a separately selected default device
        // need not match the one that owns this EGLDisplay.
        using QueryDisplay = EGLBoolean (*)(EGLDisplay, EGLint, intptr_t *);
        using QueryDevice = EGLBoolean (*)(void *, EGLint, intptr_t *);
        auto queryDisplay = reinterpret_cast<QueryDisplay>(s_egl.eglGetProcAddress("eglQueryDisplayAttribEXT"));
        auto queryDevice = reinterpret_cast<QueryDevice>(s_egl.eglGetProcAddress("eglQueryDeviceAttribEXT"));
        intptr_t device = 0, metal = 0;
        if (!queryDisplay || !queryDevice ||
            !queryDisplay(display, 0x322C /* EGL_DEVICE_EXT */, &device) ||
            !queryDevice(reinterpret_cast<void *>(device), 0x34A6 /* EGL_METAL_DEVICE_ANGLE */, &metal))
            return EGL_NO_IMAGE_KHR;
        id<MTLDevice> metalDevice = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(metal));
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        // These imported images receive CPU uploads as well as GPU rendering.
        // Opt out of Metal's opaque/compressed layout for this interop path.
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.allowGPUOptimizedContents = NO;
        id<MTLTexture> texture = [metalDevice newTextureWithDescriptor:descriptor];
        if (!texture) return EGL_NO_IMAGE_KHR;
        const EGLint attributes[] = {0x345D /* EGL_TEXTURE_INTERNAL_FORMAT_ANGLE */,
                                     static_cast<EGLint>(internalFormat), EGL_NONE};
        // ANGLE retains the Metal texture for the image's lifetime.
        return s_egl.eglCreateImageKHR(display, EGL_NO_CONTEXT,
            0x34A7 /* EGL_METAL_TEXTURE_ANGLE */, (__bridge void *)texture, attributes);
    }
}
