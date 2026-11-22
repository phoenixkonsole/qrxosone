/*
 * QRX OS One 64 runtime smoke test for the native Gecko port.
 *
 * Build with the m68k64-unknown-aros clang++ wrapper and the AROS 64 SDK:
 *   clang++ --sysroot /path/to/AROS/Development -std=c++17 -O2 \
 *       -pthread -o GeckoRuntime gecko-runtime.cpp
 * With collect-aros in PATH, place GeckoRuntime in C: on the guest image.
 * Run C:GeckoRuntime on EC64 with two configured cores and capture the
 * console. A build failure identifies a missing toolchain/runtime feature.
 * PASS checks the C++ runtime and guest thread API, not core migration.
 */

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

static_assert(sizeof(void *) == 8 && sizeof(long) == 8, "requires LP64");

namespace {
std::atomic<unsigned> tls_destructors{0};

struct ThreadState {
    unsigned value = 0;

    ~ThreadState()
    {
        ++tls_destructors;
    }
};

thread_local ThreadState state;
}

int main()
{
    state.value = 42;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    bool passed[2]{};
    std::thread workers[2];

    try {
        for (unsigned i = 0; i < 2; ++i) {
            workers[i] = std::thread([&, i] {
                ++ready;
                while (!start.load())
                    std::this_thread::yield();
                try {
                    if (state.value != 0)
                        return;
                    state.value = i + 1;
                    throw std::runtime_error("guest exception");
                } catch (const std::runtime_error &) {
                    passed[i] = state.value == i + 1;
                }
            });
        }
    } catch (const std::exception &error) {
        start = true;
        for (auto &worker : workers)
            if (worker.joinable())
                worker.join();
        std::printf("FAIL: thread creation: %s\n", error.what());
        return 20;
    }

    while (ready.load() != 2)
        std::this_thread::yield();
    start = true;
    for (auto &worker : workers)
        worker.join();

    if (!passed[0] || !passed[1] || state.value != 42 || tls_destructors.load() != 2) {
        std::puts("FAIL: C++ exceptions or thread-local storage");
        return 20;
    }
    std::puts("PASS: C++ exceptions, guest threads and thread-local storage");
    return 0;
}
