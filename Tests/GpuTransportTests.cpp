#include "ThirdParty/AndroidQemuCompat/qemu/gpu.h"
#include <array>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

static void loopback(void *, Android51GpuStream *stream) {
    std::array<unsigned char, 257> bytes{};
    for (;;) {
        const int count = android51_gpu_stream_read(stream, bytes.data(), bytes.size());
        if (count <= 0) break;
        if (!android51_gpu_stream_write(stream, bytes.data(), count)) break;
    }
}

static void roundtrip() {
    auto *a = android51_gpu_open();
    auto *b = android51_gpu_open();
    assert(a && b);
    std::vector<unsigned char> sent(200000), got(sent.size());
    for (size_t i = 0; i < sent.size(); ++i) sent[i] = (i * 13) & 255;
    size_t written = 0, read = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (read < sent.size()) {
        assert(std::chrono::steady_clock::now() < deadline);
        if (written < sent.size()) {
            int n = android51_gpu_send(a, sent.data() + written, sent.size() - written);
            assert(n > 0 || n == -2);
            if (n > 0) written += n;
        }
        int n = android51_gpu_receive(a, got.data() + read, got.size() - read);
        assert(n > 0 || n == -2);
        if (n > 0) read += n;
        std::this_thread::yield();
    }
    assert(got == sent);
    unsigned char byte = 0;
    assert(android51_gpu_receive(b, &byte, 1) == -2);
    assert(android51_gpu_send(b, nullptr, 1) == -1);
    assert(android51_gpu_send(b, nullptr, 0) == 0);
    android51_gpu_close(a);
    android51_gpu_close(b);
}

static void blocked_writer(void *opaque, Android51GpuStream *stream) {
    auto *ended = static_cast<std::atomic<bool> *>(opaque);
    std::vector<unsigned char> bytes(1024 * 1024, 0x5a);
    android51_gpu_stream_write(stream, bytes.data(), bytes.size());
    ended->store(true);
}

int main() {
    assert(!android51_gpu_ready());
    assert(!android51_gpu_open());
    Android51GpuBackend backend{ANDROID51_GPU_ABI, sizeof(Android51GpuBackend), nullptr, loopback};
    assert(android51_gpu_configure(&backend));
    assert(android51_gpu_ready());
    roundtrip();
    std::atomic<bool> ended{false};
    backend.opaque = &ended;
    backend.run = blocked_writer;
    assert(android51_gpu_configure(&backend));
    auto *stream = android51_gpu_open();
    assert(stream);
    assert(!android51_gpu_configure(nullptr)); // No replacement during use.
    android51_gpu_close(stream); // Cancels even a full reply queue.
    assert(ended.load());
    assert(android51_gpu_configure(nullptr));
    assert(!android51_gpu_ready());
    backend.abi = 999;
    assert(!android51_gpu_configure(&backend));
}
