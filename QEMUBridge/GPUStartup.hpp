#pragma once
#include "ThirdParty/AndroidQemuCompat/qemu/gpu.h"
#include <cstdio>
#include <string>

namespace emu {
inline bool initializeGuestGPU(decltype(&android51_gpu_start) start,
    decltype(&android51_gpu_stop) stop, void **libraries, uint32_t width, uint32_t height,
    void *(*resolve)(void *, unsigned, const char *),
    void (*post)(void *, const uint8_t *, uint32_t, uint32_t), void *context,
    char *error, size_t capacity) {
    if (!start || !stop || !libraries || !libraries[0] || !libraries[1] || !libraries[2]) {
        if (error && capacity) std::snprintf(error, capacity,
            "ANGLE libraries or engine GPU entry points are unavailable");
        return false;
    }
    return start(width, height, true, resolve, libraries, post, context, error, capacity);
}
inline std::string guestKernelArguments(bool gpuReady) {
    return std::string("qemu=1 console=ttyS0 androidboot.console=ttyS0 androidboot.hardware=goldfish qemu.gles=") +
        (gpuReady ? "1" : "0") + " android.qemud=1";
}
}
