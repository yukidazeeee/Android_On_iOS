#include "Renderer.h"
#include "ThirdParty/AndroidQemuCompat/qemu/gpu.h"
#include "FrameBuffer.h"
#include "RenderThreadInfo.h"
#include "RenderControl.h"
#include "EGLDispatch.h"
#include "GLESv1Dispatch.h"
#include "GLESv2Dispatch.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

bool ae_gpu_dispatch(AEGpuResolve, void *, bool);
namespace {
constexpr size_t max_packet = 64 * 1024 * 1024;
std::mutex render_mutex;
AEGpuPost post_callback;
void *post_context;
bool initialized;

uint32_t le32(const unsigned char *b) {
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
bool read_exact(Android51GpuStream *s, void *buffer, size_t size) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    while (size) {
        int count = android51_gpu_stream_read(s, bytes, size);
        if (count <= 0) return false;
        bytes += count;
        size -= count;
    }
    return true;
}
// Buffer replies under the render lock, then deliver outside it. A guest
// that stops reading must not hold the renderer's shared resource lock.
class ReplyStream final : public IOStream {
    std::vector<unsigned char> allocation;
public:
    std::vector<unsigned char> reply;
    ReplyStream() : IOStream(4096) {}
    void *allocBuffer(size_t size) override {
        if (size > max_packet) throw std::runtime_error("GPU reply too large");
        allocation.assign(size, 0);
        return allocation.data();
    }
    int commitBuffer(size_t size) override {
        if (size > allocation.size() || size > max_packet - reply.size())
            throw std::runtime_error("GPU reply overflow");
        reply.insert(reply.end(), allocation.begin(), allocation.begin() + size);
        return 0;
    }
    const unsigned char *readFully(void *, size_t) override { throw std::runtime_error("Unexpected decoder read"); }
    const unsigned char *read(void *, size_t *) override { throw std::runtime_error("Unexpected decoder read"); }
    int writeFully(const void *bytes, size_t size) override {
        if (size > max_packet - reply.size()) throw std::runtime_error("GPU reply overflow");
        auto *b = static_cast<const unsigned char *>(bytes);
        reply.insert(reply.end(), b, b + size);
        return 0;
    }
    void forceStop() override {}
};
void run(void *, Android51GpuStream *stream) {
    RenderThreadInfo info;
    info.m_glDec.initGL(gles1_dispatch_get_proc_func, nullptr);
    info.m_gl2Dec.initGL(gles2_dispatch_get_proc_func, nullptr);
    initRenderControlContext(&info.m_rcDec);
    try {
        unsigned char flags[4];
        if (read_exact(stream, flags, sizeof(flags)) && le32(flags) == 0) {
            for (;;) {
                unsigned char header[8];
                if (!read_exact(stream, header, sizeof(header))) break;
                size_t size = le32(header + 4);
                if (size < 8 || size > max_packet) break;
                std::vector<unsigned char> packet(size);
                memcpy(packet.data(), header, 8);
                if (!read_exact(stream, packet.data() + 8, size - 8)) break;
                ReplyStream output;
                size_t consumed;
                {
                    std::lock_guard<std::mutex> lock(render_mutex);
                    consumed = info.m_glDec.decode(packet.data(), size, &output);
                    if (!consumed) consumed = info.m_gl2Dec.decode(packet.data(), size, &output);
                    if (!consumed) consumed = info.m_rcDec.decode(packet.data(), size, &output);
                    output.flush();
                }
                if (consumed != size) break;
                if (!output.reply.empty() && !android51_gpu_stream_write(stream, output.reply.data(), output.reply.size())) break;
            }
        }
    } catch (const std::exception &error) {
        fprintf(stderr, "AndroidEmu GPU connection: %s\n", error.what());
    }
    std::lock_guard<std::mutex> lock(render_mutex);
    FrameBuffer::getFB()->bindContext(0, 0, 0);
    FrameBuffer::getFB()->drainWindowSurface();
    FrameBuffer::getFB()->drainRenderContext();
    s_egl.eglReleaseThread();
}
void posted(void *, int width, int height, int, int format, int type, unsigned char *pixels) {
    if (post_callback && format == GL_RGBA && type == GL_UNSIGNED_BYTE)
        post_callback(post_context, pixels, width, height);
}
}
extern "C" bool ae_gpu_renderer_initialize(uint32_t width, uint32_t height, bool metal,
    AEGpuResolve resolve, void *resolve_context, AEGpuPost post, void *context,
    char *error, size_t capacity) {
    std::lock_guard<std::mutex> lock(render_mutex);
    auto fail = [&](const char *message) {
        if (error && capacity) snprintf(error, capacity, "%s", message);
        return false;
    };
    if (initialized) return fail("GPU renderer already initialized");
    if (!resolve || !width || !height || width > 4096 || height > 4096)
        return fail("Invalid GPU resolver or display dimensions");
    try {
        if (!ae_gpu_dispatch(resolve, resolve_context, metal)) return fail("Required EGL/GLES entry points missing");
        if (!FrameBuffer::initialize(width, height)) return fail("EGL/GLES1/GLES2 framebuffer initialization failed");
        // Exercise image allocation/import before announcing qemu.gles=1.
        // In particular Metal cannot export EGL_GL_TEXTURE_2D_KHR images.
        for (GLenum format : {GL_RGBA, GL_RGB}) {
            auto probe = FrameBuffer::getFB()->createColorBuffer(1, 1, format);
            if (!probe) {
                FrameBuffer::finalize();
                return fail("EGL shared color-buffer image allocation/import failed");
            }
            FrameBuffer::getFB()->closeColorBuffer(probe);
        }
        post_callback = post;
        post_context = context;
        FrameBuffer::getFB()->setPostCallback(posted, nullptr);
        Android51GpuBackend backend{ANDROID51_GPU_ABI, sizeof(Android51GpuBackend), nullptr, run};
        if (!android51_gpu_configure(&backend)) {
            FrameBuffer::finalize();
            return fail("GPU transport is already in use");
        }
        initialized = true;
        return true;
    } catch (const std::exception &exception) { return fail(exception.what()); }
}
extern "C" bool ae_gpu_renderer_shutdown(void) {
    if (!android51_gpu_configure(nullptr)) return false;
    std::lock_guard<std::mutex> lock(render_mutex);
    if (initialized) FrameBuffer::finalize();
    initialized = false;
    post_callback = nullptr;
    post_context = nullptr;
    return true;
}
