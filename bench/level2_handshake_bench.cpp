// Level 2 benchmark: pure handshake compute cost, no network involved. This
// isolates "how expensive is one IK handshake" (2x X25519 keygen + 4x DH +
// a handful of AEAD/HKDF calls) from the io_uring transport cost measured
// in level 1. Wiring this into the actual UDP event loop is a level-7
// integration concern.

#include "../net/noise_ik.hpp"

#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    unsigned seconds = argc > 1 ? (unsigned)std::atoi(argv[1]) : 3;

    auto initiator_static = crypto::x25519_generate();
    auto responder_static = crypto::x25519_generate();

    // Warm up (first-call CPU feature detection / allocator warmup) so the
    // measured loop reflects steady-state cost, not cold-start noise.
    for (int i = 0; i < 100; ++i) {
        noise::noise_ik_initiator initiator(initiator_static, responder_static.pub);
        noise::noise_ik_responder responder(responder_static);
        auto msg1 = initiator.write_message1(nullptr, 0);
        responder.read_message1(msg1);
        auto msg2 = responder.write_message2(nullptr, 0);
        initiator.read_message2(msg2);
        initiator.finish();
        responder.finish();
    }

    uint64_t handshakes = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) {
            noise::noise_ik_initiator initiator(initiator_static, responder_static.pub);
            noise::noise_ik_responder responder(responder_static);
            auto msg1 = initiator.write_message1(nullptr, 0);
            responder.read_message1(msg1);
            auto msg2 = responder.write_message2(nullptr, 0);
            initiator.read_message2(msg2);
            initiator.finish();
            responder.finish();
        }
        handshakes += 1000;
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double per_sec = (double)handshakes / elapsed;
    fprintf(stderr, "[level2] handshakes=%llu elapsed=%.2fs full_handshakes_per_sec=%.0f\n",
            (unsigned long long)handshakes, elapsed, per_sec);
    fprintf(stderr, "[level2] one-sided (initiator OR responder only) ops_per_sec=%.0f\n", per_sec * 2);
    return 0;
}
