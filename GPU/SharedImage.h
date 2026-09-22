#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>

// Native Metal-backed images are shared between independent guest contexts.
// Desktop EGL backends continue to export GL textures as EGLImages.
bool ae_gpu_has_native_images(EGLDisplay display);
EGLImageKHR ae_gpu_create_native_image(EGLDisplay display, int width, int height, unsigned internalFormat);
