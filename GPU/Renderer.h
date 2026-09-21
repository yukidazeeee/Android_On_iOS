#ifndef AE_GPU_RENDERER_H
#define AE_GPU_RENDERER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* api: 0 EGL, 1 GLES1, 2 GLES2. Returned addresses must remain valid until
 * shutdown. Metal selects EGL_ANGLE_platform_angle's Metal backend. */
typedef void *(*AEGpuResolve)(void *, unsigned api, const char *name);
typedef void (*AEGpuPost)(void *, const uint8_t *rgba, uint32_t width, uint32_t height);
bool ae_gpu_renderer_initialize(uint32_t width, uint32_t height, bool metal,
    AEGpuResolve resolve, void *resolve_context, AEGpuPost post, void *post_context,
    char *error, size_t error_capacity);
/* All guest pipes must be closed before shutdown. */
bool ae_gpu_renderer_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
