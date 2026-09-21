#include "Renderer.h"
#include "EGLDispatch.h"
#include "GLESv1Dispatch.h"
#include "GLESv2Dispatch.h"
#include "NativeSubWindow.h"
#include <cstring>

EGLDispatch s_egl{};
gles1_decoder_context_t s_gles1;
gles2_decoder_context_t s_gles2;
static AEGpuResolve resolver;
static void *resolver_context;
static EGLDisplay (*platform_display)(EGLenum, void *, const EGLint *);

static EGLDisplay metal_display(EGLNativeDisplayType) {
    // EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_ANGLE,
    // EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE.
    const EGLint attributes[] = {0x3203, 0x3489, EGL_NONE};
    return platform_display(0x3202, nullptr, attributes);
}
void *gles1_dispatch_get_proc_func(const char *name, void *) {
    return resolver(resolver_context, 1, name);
}
void *gles2_dispatch_get_proc_func(const char *name, void *) {
    return resolver(resolver_context, 2, name);
}
bool ae_gpu_dispatch(AEGpuResolve resolve, void *context, bool metal) {
    resolver = resolve;
    resolver_context = context;
#include "egl_dispatch.inc"
    if (!s_egl.eglGetDisplay || !s_egl.eglInitialize || !s_egl.eglGetConfigs ||
        !s_egl.eglGetConfigAttrib || !s_egl.eglChooseConfig || !s_egl.eglBindAPI ||
        !s_egl.eglCreateContext || !s_egl.eglDestroyContext || !s_egl.eglMakeCurrent ||
        !s_egl.eglCreatePbufferSurface || !s_egl.eglDestroySurface ||
        !s_egl.eglGetCurrentContext || !s_egl.eglGetCurrentSurface ||
        !s_egl.eglQueryString || !s_egl.eglGetError || !s_egl.eglTerminate ||
        !s_egl.eglReleaseThread || !s_egl.eglCreateImageKHR || !s_egl.eglDestroyImageKHR) return false;
    if (metal) {
        platform_display = reinterpret_cast<decltype(platform_display)>(resolve(context, 0, "eglGetPlatformDisplayEXT"));
        if (!platform_display) return false;
        s_egl.eglGetDisplay = metal_display;
    }
    s_gles1.initDispatchByName(gles1_dispatch_get_proc_func, nullptr);
    s_gles2.initDispatchByName(gles2_dispatch_get_proc_func, nullptr);
    return s_gles1.glGetString && s_gles1.glEGLImageTargetTexture2DOES &&
        s_gles2.glGetString && s_gles2.glEGLImageTargetTexture2DOES &&
        s_gles2.glEGLImageTargetRenderbufferStorageOES;
}
// The renderer uses pbuffers and a post callback on both iOS and host tests.
// Desktop native window creation is deliberately unavailable.
extern "C" EGLNativeWindowType createSubWindow(FBNativeWindowType, int, int, int, int) { return {}; }
extern "C" void destroySubWindow(EGLNativeWindowType) {}
