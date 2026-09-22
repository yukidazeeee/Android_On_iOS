#include "GPU/Renderer.h"
#include "Display/GPUFrame.hpp"
#include "ThirdParty/AndroidQemuCompat/qemu/gpu.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <array>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>

struct Libraries { void *egl, *gles1, *gles2; };
static void *reject_image(void *, void *, unsigned, void *, const int *) { return nullptr; }

static std::atomic<bool> reject_readback{false};
using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void *);
static ReadPixels real_read_pixels;
static void checked_read_pixels(int x, int y, int width, int height,
                                unsigned format, unsigned type, void *pixels) {
    real_read_pixels(x, y, reject_readback.load() ? -1 : width, height, format, type, pixels);
}
static void *resolve(void *opaque, unsigned api, const char *name) {
    auto *libs = static_cast<Libraries *>(opaque);
    void *library = api == 0 ? libs->egl : api == 1 ? libs->gles1 : libs->gles2;
    void *result = dlsym(library, name);
    if (!result) {
        auto proc = reinterpret_cast<void *(*)(const char *)>(dlsym(libs->egl, "eglGetProcAddress"));
        if (proc) result = proc(name);
    }
    if (api == 2 && std::strcmp(name, "glReadPixels") == 0 && result) {
        real_read_pixels = reinterpret_cast<ReadPixels>(result);
        return reinterpret_cast<void *>(checked_read_pixels);
    }
    return result;
}
static void *resolve_without_images(void *opaque, unsigned api, const char *name) {
    if (api == 0 && std::strcmp(name, "eglCreateImageKHR") == 0)
        return reinterpret_cast<void *>(reject_image);
    return resolve(opaque, api, name);
}
static void exact_read(Android51GpuStream *stream, void *data, size_t size) {
    auto *cursor = static_cast<unsigned char *>(data);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (size) {
        assert(std::chrono::steady_clock::now() < deadline);
        int count = android51_gpu_receive(stream, cursor, size);
        assert(count > 0 || count == -2);
        if (count > 0) { cursor += count; size -= count; }
        else std::this_thread::yield();
    }
}
static void send_packet(Android51GpuStream *stream, uint32_t opcode, std::vector<uint32_t> words) {
    words.insert(words.begin(), {opcode, uint32_t(8 + words.size() * 4)});
    const auto *bytes = reinterpret_cast<const unsigned char *>(words.data());
    size_t remaining = words.size() * 4;
    while (remaining) {
        int count = android51_gpu_send(stream, bytes, remaining);
        assert(count > 0 || count == -2);
        if (count > 0) { bytes += count; remaining -= count; }
        else std::this_thread::yield();
    }
}
static uint32_t reply_word(Android51GpuStream *stream) {
    uint32_t word;
    exact_read(stream, &word, sizeof(word));
    return word;
}
static uint32_t choose_config(Android51GpuStream *stream, unsigned version, uint32_t colorFormat) {
    // EGL RGB8, pbuffer, ES1/ES2. Literal wire layout from renderControl.in.
    send_packet(stream, 10006, {52, 0x3024,8, 0x3023,8, 0x3022,8,
        0x3021,colorFormat == 0x1908 ? 8u : 0u,
        0x3033,1, 0x3040,version == 1 ? 1u : 4u, 0x3038, 52, 4, 1});
    uint32_t config = reply_word(stream);
    assert(reply_word(stream) == 1);
    return config;
}
static uint32_t compile_shader(Android51GpuStream *stream, uint32_t type, const char *text) {
    send_packet(stream, 2074, {type});
    uint32_t shader = reply_word(stream);
    assert(shader);
    size_t bytes = std::strlen(text) + 1;
    size_t padded = (bytes + 3) & ~size_t(3);
    std::vector<uint32_t> words{shader, uint32_t(padded)};
    words.resize(2 + padded / 4, 0);
    std::memcpy(words.data() + 2, text, bytes);
    words.push_back(uint32_t(bytes));
    send_packet(stream, 2254, words);
    send_packet(stream, 2068, {shader});
    send_packet(stream, 2115, {shader,0x8b81,4});
    assert(reply_word(stream) == 1);
    return shader;
}
static void shader_and_fbo(Android51GpuStream *stream) {
    uint32_t vertex = compile_shader(stream, 0x8b31,
        "attribute vec4 position; void main(){gl_Position=position;}");
    uint32_t fragment = compile_shader(stream, 0x8b30,
        "precision mediump float; void main(){gl_FragColor=vec4(0.,1.,0.,1.);}");
    send_packet(stream, 2073, {});
    uint32_t program = reply_word(stream);
    assert(program);
    send_packet(stream, 2049, {program,vertex});
    send_packet(stream, 2049, {program,fragment});
    send_packet(stream, 2137, {program});
    send_packet(stream, 2112, {program,0x8b82,4});
    assert(reply_word(stream) == 1);
    send_packet(stream, 2178, {program});

    send_packet(stream, 2101, {1,4});
    uint32_t texture = reply_word(stream);
    assert(texture);
    send_packet(stream, 2054, {0x0de1,texture});
    send_packet(stream, 2153, {0x0de1,0,0x1908,2,2,0,0x1908,0x1401,0});
    send_packet(stream, 2099, {1,4});
    uint32_t fbo = reply_word(stream);
    assert(fbo);
    send_packet(stream, 2052, {0x8d40,fbo});
    send_packet(stream, 2095, {0x8d40,0x8ce0,0x0de1,texture,0});
    send_packet(stream, 2062, {0x8d40});
    assert(reply_word(stream) == 0x8cd5); // Actual GL_FRAMEBUFFER_COMPLETE.
    send_packet(stream, 2064, {0,0x3f800000,0,0x3f800000});
    send_packet(stream, 2063, {0x4000});
    send_packet(stream, 2140, {0,0,1,1,0x1908,0x1401,4});
    assert(reply_word(stream) == 0xff00ff00);
    send_packet(stream, 2108, {});
    assert(reply_word(stream) == 0);
    send_packet(stream, 2052, {0x8d40,0});
}
static void assert_posted_corners(uint32_t color);
static void draw_context(Android51GpuStream *stream, unsigned version, uint32_t colorFormat) {
    uint32_t config = choose_config(stream, version, colorFormat);
    send_packet(stream, 10008, {config, 0, version});
    uint32_t context = reply_word(stream);
    assert(context);
    send_packet(stream, 10010, {config,16,16});
    uint32_t surface = reply_word(stream);
    assert(surface);
    send_packet(stream, 10017, {context,surface,surface});
    assert(reply_word(stream) == 1);
    send_packet(stream, 10012, {16,16,colorFormat});
    uint32_t color = reply_word(stream);
    assert(color);
    send_packet(stream, 10015, {surface,color});
    // IEEE754 1.0f, 0.0f, 0.0f, 1.0f; clear and read actual pixels.
    send_packet(stream, version == 1 ? 1025 : 2064, {0x3f800000,0,0,0x3f800000});
    send_packet(stream, version == 1 ? 1069 : 2063, {0x4000});
    send_packet(stream, version == 1 ? 1143 : 2140, {0,0,1,1,0x1908,0x1401,4});
    std::array<unsigned char,4> pixel{};
    exact_read(stream, pixel.data(), pixel.size());
    assert((pixel == std::array<unsigned char,4>{255,0,0,255}));
    // Exercise the guest window -> shared EGLImage -> renderer context path.
    send_packet(stream, 10016, {surface});
    assert(reply_word(stream) == 0);
    send_packet(stream, 10023, {color,0,0,1,1,0x1908,0x1401,4});
    assert(reply_word(stream) == 0xff0000ff);
    // An asymmetric image distinguishes vertical flips from channel swaps.
    send_packet(stream, version == 1 ? 1092 : 2090, {0x0c11});
    auto quadrant = [&](uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b) {
        send_packet(stream, version == 1 ? 1148 : 2144, {x,y,8,8});
        send_packet(stream, version == 1 ? 1025 : 2064, {r,g,b,0x3f800000});
        send_packet(stream, version == 1 ? 1069 : 2063, {0x4000});
    };
    quadrant(0,8,0,0x3f800000,0); // top left green
    quadrant(8,8,0,0,0x3f800000); // top right blue
    quadrant(8,0,0x3f800000,0x3f800000,0x3f800000); // bottom right white
    send_packet(stream, version == 1 ? 1088 : 2086, {0x0c11});
    send_packet(stream, 10016, {surface});
    assert(reply_word(stream) == 0);
    assert_posted_corners(color);
    if (version == 2) shader_and_fbo(stream);
    send_packet(stream, 10017, {0,0,0});
    assert(reply_word(stream) == 1);
    send_packet(stream, 10011, {surface});
    send_packet(stream, 10009, {context});
    send_packet(stream, 10014, {color});
}

static std::mutex frame_mutex;
static std::vector<unsigned char> last_frame;
static unsigned posted_count;
static void posted(void *, const uint8_t *pixels, uint32_t width, uint32_t height) {
    assert(width == 16 && height == 16);
    std::lock_guard<std::mutex> lock(frame_mutex);
    last_frame.assign(pixels, pixels + width * height * 4);
    ++posted_count;
}
static void assert_posted_corners(uint32_t color) {
    // SurfaceFlinger's post arrives on a different guest pipe/host thread.
    auto *consumer = android51_gpu_open();
    assert(consumer);
    uint32_t flags = 0;
    assert(android51_gpu_send(consumer, &flags, sizeof(flags)) == sizeof(flags));
    send_packet(consumer, 10018, {color});
    send_packet(consumer, 10000, {});
    assert(reply_word(consumer) == 1);
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        assert(last_frame.size() == 16 * 16 * 4);
        std::vector<uint8_t> display(last_frame.size());
        emu::gpuFrameToBGRA(last_frame.data(), display.data(), 16 * 16);
        auto corner = [&](size_t x, size_t y, std::array<uint8_t,4> expected) {
            assert(std::memcmp(display.data() + (y * 16 + x) * 4, expected.data(), 4) == 0);
        };
        corner(0,0,{0,255,0,255});
        corner(15,0,{255,0,0,255});
        corner(0,15,{0,0,255,255});
        corner(15,15,{255,255,255,255});
    }
    unsigned count;
    { std::lock_guard<std::mutex> lock(frame_mutex); count = posted_count; }
    reject_readback.store(true);
    send_packet(consumer, 10018, {color});
    send_packet(consumer, 10000, {});
    assert(reply_word(consumer) == 1);
    { std::lock_guard<std::mutex> lock(frame_mutex); assert(posted_count == count); }
    reject_readback.store(false);
    send_packet(consumer, 10018, {color});
    send_packet(consumer, 10000, {});
    assert(reply_word(consumer) == 1);
    { std::lock_guard<std::mutex> lock(frame_mutex); assert(posted_count == count + 1); }
    android51_gpu_close(consumer);
}

static void colorbuffer(Android51GpuStream *stream) {
    send_packet(stream, 10012, {16,16,0x1908});
    uint32_t color = reply_word(stream);
    assert(color);
    std::vector<uint32_t> data{color,0,0,16,16,0x1908,0x1401,1024};
    data.insert(data.end(), 256, 0xff00ff00); // RGBA green.
    send_packet(stream, 10024, data);
    assert(reply_word(stream) == 0);
    send_packet(stream, 10018, {color});
    send_packet(stream, 10000, {}); // Ordered round-trip after post.
    assert(reply_word(stream) == 1);
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        assert(last_frame.size() == 1024);
        for (size_t i = 0; i < last_frame.size(); i += 4) {
            assert(last_frame[i] == 0 && last_frame[i+1] == 255 &&
                last_frame[i+2] == 0 && last_frame[i+3] == 255);
        }
    }
    send_packet(stream, 10014, {color});
}
static void odd_width_rgb(Android51GpuStream *stream) {
    for (uint32_t width : {1u, 3u, 5u}) {
        constexpr uint32_t height = 3;
        send_packet(stream, 10012, {width,height,0x1907});
        uint32_t color = reply_word(stream);
        assert(color);
        send_packet(stream, 10023, {color,0,0,width,height,0x1908,0x1401,width*height*4});
        for (uint32_t pixel = 0; pixel < width * height; ++pixel)
            assert(reply_word(stream) == 0xff000000);
        // Tight RGB rows: 3, 9 and 15 bytes, all different from alignment 4.
        uint32_t byteCount = width * height * 3;
        uint32_t padded = (byteCount + 3) & ~3u;
        std::vector<uint32_t> update{color,0,0,width,height,0x1907,0x1401,padded};
        update.resize(8 + padded / 4, 0);
        auto *pixels = reinterpret_cast<unsigned char *>(update.data() + 8);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) pixels[(y * width + x) * 3 + y] = 255;
        send_packet(stream, 10024, update);
        assert(reply_word(stream) == 0);
        send_packet(stream, 10023, {color,0,0,width,height,0x1908,0x1401,width*height*4});
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                assert(reply_word(stream) == (0xff000000u | (0xffu << (y * 8))));
        send_packet(stream, 10014, {color});
    }
}

int main() {
    Libraries libs{dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL),
        dlopen("libGLESv1_CM.so.1", RTLD_NOW | RTLD_LOCAL),
        dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL)};
    assert(libs.egl && libs.gles1 && libs.gles2);
    char error[512]{};
    // A loaded EGL library with unusable shared images must not enable GPU.
    assert(!ae_gpu_renderer_initialize(16, 16, false, resolve_without_images,
        &libs, posted, nullptr, error, sizeof(error)));
    assert(std::strstr(error, "shared color-buffer") != nullptr);
    assert(!android51_gpu_ready());
    assert(ae_gpu_renderer_shutdown());
    if (!ae_gpu_renderer_initialize(16, 16, false, resolve, &libs, posted, nullptr, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    auto *stream = android51_gpu_open();
    assert(stream);
    // Zero client flags, rcGetRendererVersion opcode 10000, packet length 8.
    const unsigned char bytes[] = {0,0,0,0, 0x10,0x27,0,0, 8,0,0,0};
    for (unsigned char byte : bytes) assert(android51_gpu_send(stream, &byte, 1) == 1);
    std::array<unsigned char,4> reply{};
    exact_read(stream, reply.data(), reply.size());
    assert((reply == std::array<unsigned char,4>{1,0,0,0}));
    for (unsigned version : {1u, 2u}) {
        draw_context(stream, version, 0x1908); // RGBA
        draw_context(stream, version, 0x1907); // RGB / RGB565 image semantics
    }
    odd_width_rgb(stream);
    colorbuffer(stream);
    assert(!ae_gpu_renderer_shutdown());
    android51_gpu_close(stream);
    assert(ae_gpu_renderer_shutdown());
}
