/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ANDROID51_GPU_H
#define ANDROID51_GPU_H
#ifdef AE_HAS_EMUGL
#include <Bridge.h>
#else
static inline int ae_gpu_init(unsigned w, unsigned h) { return 0; }
static inline void *ae_gpu_open(void) { return NULL; }
static inline void ae_gpu_close(void *s) {}
static inline int ae_gpu_fd(void *s) { return -1; }
static inline int ae_gpu_send(void *s, const void *p, size_t n) { return -4; }
static inline int ae_gpu_receive(void *s, void *p, size_t n) { return -4; }
static inline unsigned ae_gpu_poll(void *s) { return 4; }
static inline void ae_gpu_invalidate_frame(void) {}
static inline int ae_gpu_frame_region(uint8_t *p, size_t n, unsigned *first, unsigned *rows) { return 0; }
static inline size_t ae_gpu_diagnostics_copy(char *out, size_t capacity) {
    if (out && capacity) { out[0] = '\0'; } return 0;
}
static inline void ae_gpu_diagnostics_clear(void) {}
static inline void ae_gpu_diagnostics_mark(const char *label) { (void)label; }
static inline void ae_gpu_diagnostics_note_bgra_frame(
        const uint8_t *p, size_t stride, unsigned fw, unsigned fh,
        unsigned x, unsigned y, unsigned w, unsigned h) {
    (void)p; (void)stride; (void)fw; (void)fh;
    (void)x; (void)y; (void)w; (void)h;
}
#endif
#endif
