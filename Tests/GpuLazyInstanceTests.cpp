#include "emugl/common/lazy_instance.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

static std::atomic<unsigned> constructions{0};
struct Payload {
    unsigned values[128];
    Payload() {
        ++constructions;
        for (unsigned i = 0; i < 128; ++i) {
            values[i] = i * 17 + 3;
            std::this_thread::yield();
        }
    }
};

int main() {
    // Race construction and publication repeatedly, using fresh zero state.
    for (unsigned round = 0; round < 32; ++round) {
        emugl::LazyInstance<Payload> instance = LAZY_INSTANCE_INIT;
        assert(!instance.hasInstance());
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < 16; ++worker) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (unsigned repeat = 0; repeat < 16; ++repeat) {
                    const auto& payload = instance.get();
                    for (unsigned i = 0; i < 128; ++i) assert(payload.values[i] == i * 17 + 3);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        assert(constructions == round + 1);
        assert(instance.hasInstance());
        instance.get().~Payload();
    }
}
