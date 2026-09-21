#include "QEMUBridge/GPUStartup.hpp"
#include <cassert>
#include <cstring>

static int attempts;
static bool available;
static bool initialize(uint32_t, uint32_t, bool, void *(*)(void *, unsigned, const char *),
    void *, void (*)(void *, const uint8_t *, uint32_t, uint32_t), void *, char *error, size_t size) {
    ++attempts;
    if (!available && size) { std::strncpy(error, "EGL initialization failed", size - 1); error[size-1] = 0; }
    return available;
}
static bool stop() { return true; }
static void *resolve(void *, unsigned, const char *) { return nullptr; }
int main() {
    int handles[3];
    void *libraries[] = {&handles[0], &handles[1], &handles[2]};
    char error[128]{};
    auto boot = [&](decltype(&android51_gpu_start) start = initialize) {
        return emu::initializeGuestGPU(start, stop, libraries, 540, 960, resolve,
                                      nullptr, nullptr, error, sizeof(error));
    };
    libraries[0] = nullptr;
    assert(!boot());
    assert(attempts == 0);
    assert(!boot(nullptr));
    libraries[0] = &handles[0];
    assert(!boot());
    assert(attempts == 1);
    assert(std::strstr(error, "EGL initialization failed"));
    assert(emu::guestKernelArguments(false).find("qemu.gles=0") != std::string::npos);
    available = true;
    assert(boot());
    assert(emu::guestKernelArguments(true).find("qemu.gles=1") != std::string::npos);
}
