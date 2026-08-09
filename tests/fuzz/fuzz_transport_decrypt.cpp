// Fuzzes the post-handshake data path: raw wire bytes into
// connection::decrypt_and_reassemble().
//
// Everything here runs on bytes an off-path attacker can send to an
// established session's conn_id. The AEAD tag should reject essentially all
// of it, but the length arithmetic and the replay-window check both run
// BEFORE decryption succeeds - so those are reachable with arbitrary input
// and must not read out of bounds or desync the window.
#include "../../net/transport_connection.hpp"

#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto ik = crypto::x25519_generate();
    static auto rk = crypto::x25519_generate();
    noise::noise_ik_initiator initiator(ik, rk.pub);
    noise::noise_ik_responder responder(rk);
    auto m1 = initiator.write_message1(nullptr, 0);
    responder.read_message1(m1);
    auto m2 = responder.write_message2(nullptr, 0);
    initiator.read_message2(m2);
    auto ikeys = initiator.finish();
    auto rkeys = responder.finish();

    transport::connection<> sender(std::move(ikeys.send), std::move(ikeys.recv));
    transport::connection<> receiver(std::move(rkeys.send), std::move(rkeys.recv));

    // Establish genuine traffic first, so the replay window and reassembler
    // hold real state that a forged packet could try to corrupt.
    std::vector<uint8_t> real(128, 0x5A);
    auto good = sender.encrypt_message(real.data(), real.size());
    std::vector<uint8_t> out;
    uint32_t mid = 0;
    for (auto& p : good) receiver.decrypt_and_reassemble(p.data(), p.size(), out, mid);

    const uint64_t auth_before = receiver.auth_failures();

    std::vector<uint8_t> forged;
    uint32_t forged_msg_id = 0;
    auto res = receiver.decrypt_and_reassemble(data, size, forged, forged_msg_id);

    // Arbitrary bytes must never authenticate. Forging an AEAD tag without
    // the key is the thing the whole transport rests on, so a `completed`
    // here would be a genuine break, not a bug.
    if (res == transport::result::completed && size > 0) {
        // Only legitimate if the fuzzer happened to reproduce a real packet
        // byte-for-byte, which it cannot without the key.
        __builtin_trap();
    }

    // Rejected input must be accounted for, not silently swallowed.
    if (res == transport::result::auth_failed && receiver.auth_failures() <= auth_before) __builtin_trap();

    // A genuine packet must still work afterwards: a forged packet must not
    // have desynced the replay window or the reassembler (this is exactly
    // the check/commit split's job - see replay_window.hpp).
    auto good2 = sender.encrypt_message(real.data(), real.size());
    std::vector<uint8_t> out2;
    bool delivered = false;
    for (auto& p : good2) {
        if (receiver.decrypt_and_reassemble(p.data(), p.size(), out2, mid) == transport::result::completed)
            delivered = true;
    }
    if (!delivered) __builtin_trap();
    if (out2 != real) __builtin_trap();
    return 0;
}
