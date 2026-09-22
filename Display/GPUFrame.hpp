#pragma once
#include <cstddef>
#include <cstdint>

namespace emu {
// EmuGL's window-to-color-buffer blit already converts GL's bottom-up rows
// to Android buffer order. CPU gralloc updates use the same top-down order.
inline void gpuFrameToBGRA(const uint8_t *rgba, uint8_t *bgra, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        bgra[i * 4] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
}
}
