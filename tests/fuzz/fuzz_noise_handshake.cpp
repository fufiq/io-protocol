// Fuzzes the Noise_IK responder against arbitrary message1 bytes.
//
// This is the pre-authentication attack surface: anything that reaches a
// listening endpoint gets parsed here before any identity is established.
// mac1 filtering is supposed to make the expensive DH unreachable for a
// sender who doesn't know the responder's public key, and read_message1
// must reject everything malformed via handshake_error rather than by
// reading out of bounds or asserting.
#include "../../net/noise_ik.hpp"

#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto responder_static = crypto::x25519_generate();

    std::vector<uint8_t> msg(data, data + size);

    // The cheap pre-DH filter must never fault on arbitrary bytes.
    bool mac1_ok = noise::noise_ik_responder::verify_mac1(responder_static.pub, msg.data(), msg.size());

    // And the full parse must either succeed or throw handshake_error -
    // never anything else, and never crash.
    try {
        noise::noise_ik_responder responder(responder_static);
        auto result = responder.read_message1(msg);
        // If a random input ever authenticated, that would be a forgery:
        // it can only happen with a real key, so reaching here at all with
        // fuzzer bytes means mac1/AEAD accepted something it shouldn't.
        (void)result;
        auto m2 = responder.write_message2(nullptr, 0);
        if (m2.empty()) __builtin_trap();
        auto keys = responder.finish();
        (void)keys;
    } catch (const noise::handshake_error&) {
        // The expected outcome for essentially every fuzzer input.
    }
    (void)mac1_ok;
    return 0;
}
