#include <doctest/doctest.h>

#include "../net/reliability.hpp"

#include <chrono>

using namespace reliability;
using namespace std::chrono_literals;

namespace {

transport::connection<> make_loopback_connection() {
    auto initiator_static = crypto::x25519_generate();
    auto responder_static = crypto::x25519_generate();

    noise::noise_ik_initiator initiator(initiator_static, responder_static.pub);
    noise::noise_ik_responder responder(responder_static);
    auto msg1 = initiator.write_message1(nullptr, 0);
    responder.read_message1(msg1);
    auto msg2 = responder.write_message2(nullptr, 0);
    initiator.read_message2(msg2);

    noise::transport_keys keys = initiator.finish();
    return transport::connection<>(std::move(keys.send), std::move(keys.recv));
}

struct connected_pair {
    transport::connection<> a;
    transport::connection<> b;
};

connected_pair make_connected_pair() {
    auto initiator_static = crypto::x25519_generate();
    auto responder_static = crypto::x25519_generate();

    noise::noise_ik_initiator initiator(initiator_static, responder_static.pub);
    noise::noise_ik_responder responder(responder_static);
    auto msg1 = initiator.write_message1(nullptr, 0);
    responder.read_message1(msg1);
    auto msg2 = responder.write_message2(nullptr, 0);
    initiator.read_message2(msg2);

    noise::transport_keys ik = initiator.finish();
    noise::transport_keys rk = responder.finish();
    return connected_pair{transport::connection<>(std::move(ik.send), std::move(ik.recv)),
                          transport::connection<>(std::move(rk.send), std::move(rk.recv))};
}

} // namespace

TEST_CASE("encode_sack/decode_sack round-trips exactly") {
    assembly::ack_info info{123456, 5, 0b10101, false};
    auto wire = encode_sack(info);
    REQUIRE(wire.size() == sack_wire_len);
    auto back = decode_sack(wire.data());
    CHECK(back.msg_id == info.msg_id);
    CHECK(back.frag_count == info.frag_count);
    CHECK(back.received_mask == info.received_mask);
}

TEST_CASE("decode_sack infers `completed` from a full bitmask") {
    assembly::ack_info full{1, 3, 0b111, false /* not set by encode_sack, irrelevant */};
    auto wire = encode_sack(full);
    CHECK(decode_sack(wire.data()).completed);

    assembly::ack_info partial{1, 3, 0b011, false};
    auto wire2 = encode_sack(partial);
    CHECK_FALSE(decode_sack(wire2.data()).completed);
}

TEST_CASE("an acked fragment is never retransmitted") {
    auto conn = make_loopback_connection();
    reliable_sender sender(conn);

    std::vector<uint8_t> msg = {1, 2, 3};
    auto packets = sender.send_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 1);
    CHECK(sender.pending_count() == 1);

    sender.handle_ack({0, 1, 0b1, true});
    CHECK(sender.pending_count() == 0);

    auto t0 = std::chrono::steady_clock::now();
    sender.collect_retransmits(t0); // anchor
    auto retransmits = sender.collect_retransmits(t0 + std::chrono::seconds(1));
    CHECK(retransmits.empty());
}

TEST_CASE("an unacked fragment is retransmitted after the initial timeout, re-encrypted with a fresh nonce") {
    auto pair = make_connected_pair();
    reliable_sender sender(pair.a, 20ms, 100ms, 5);

    std::vector<uint8_t> msg = {1, 2, 3};
    auto packets = sender.send_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 1);

    auto t0 = std::chrono::steady_clock::now();
    sender.collect_retransmits(t0); // anchor
    CHECK(sender.collect_retransmits(t0 + 50ms).empty());

    auto retransmits = sender.collect_retransmits(t0 + 120ms);
    REQUIRE(retransmits.size() == 1);

    // Must NOT be the exact same bytes: a byte-identical retransmit reuses
    // the original nonce, which replay::window would then reject as a
    // replay of a packet it's already committed - see
    // transport_connection.hpp's encrypt_fragment comment. The retry has to
    // look like a brand-new packet on the wire.
    CHECK(retransmits[0] != packets[0]);

    // But it must still decrypt to the same plaintext through the peer's
    // connection - re-encryption must be transparent to the receiver.
    std::vector<uint8_t> out_original, out_retransmit;
    auto res1 = pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out_original);
    REQUIRE(res1 == transport::result::completed);
    CHECK(out_original == msg);

    auto res2 = pair.b.decrypt_and_reassemble(retransmits[0].data(), retransmits[0].size(), out_retransmit);
    CHECK(res2 == transport::result::duplicate_of_completed);
}

TEST_CASE("a fragment that never gets acked gives up after max_retries and stops being tracked") {
    auto conn = make_loopback_connection();
    reliable_sender sender(conn, 20ms, 50ms, /*max_retries=*/3);

    std::vector<uint8_t> msg = {9};
    sender.send_message(msg.data(), msg.size());

    auto t = std::chrono::steady_clock::now();
    sender.collect_retransmits(t);

    // First timeout fires the 1st retransmit; subsequent (backed-off) timeouts fire the 2nd and
    // 3rd; the 4th time its wheel entry would fire, max_retries is already exhausted and it's
    // dropped instead. Walk time forward generously to sweep through all of them.
    size_t retransmit_count = 0;
    for (int i = 0; i < 200 && sender.pending_count() > 0; ++i) {
        t += 50ms;
        auto r = sender.collect_retransmits(t);
        retransmit_count += r.size();
    }

    CHECK(sender.pending_count() == 0);
    CHECK(sender.gave_up_count() == 1);
    CHECK(retransmit_count == 3);
}

TEST_CASE("partial ack of a multi-fragment message only stops the acked fragments from retransmitting") {
    auto pair = make_connected_pair();
    reliable_sender sender(pair.a, 20ms, 50ms, 5);

    std::vector<uint8_t> msg(assembly::max_fragment_payload + 10); // 2 fragments
    auto packets = sender.send_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 2);
    CHECK(sender.pending_count() == 2);

    // Ack only fragment 0.
    sender.handle_ack({0, 2, 0b01, false});
    CHECK(sender.pending_count() == 1);

    auto t0 = std::chrono::steady_clock::now();
    sender.collect_retransmits(t0);
    auto retransmits = sender.collect_retransmits(t0 + 60ms);
    REQUIRE(retransmits.size() == 1);

    // Only fragment 1 comes back (fragment 0 was acked); it must decrypt to
    // the same payload as the original fragment 1, but with different wire
    // bytes (fresh nonce on retry - see the test above).
    CHECK(retransmits[0] != packets[1]);

    assembly::fragment_header hdr = assembly::decode_header(packets[1].data() + transport::nonce_len);
    CHECK(hdr.frag_index == 1);

    std::vector<uint8_t> out;
    auto res = pair.b.decrypt_and_reassemble(retransmits[0].data(), retransmits[0].size(), out);
    CHECK(res == transport::result::incomplete); // fragment 0 was never delivered to pair.b
}

TEST_CASE("has_pending reflects per-message completion, not just the global pending count") {
    auto conn = make_loopback_connection();
    reliable_sender sender(conn);

    std::vector<uint8_t> msg(assembly::max_fragment_payload + 10); // msg_id 0, 2 fragments
    sender.send_message(msg.data(), msg.size());
    CHECK(sender.has_pending(0, 2));

    sender.handle_ack({0, 2, 0b01, false}); // only fragment 0 acked
    CHECK(sender.has_pending(0, 2)); // fragment 1 still outstanding

    sender.handle_ack({0, 2, 0b11, true});
    CHECK_FALSE(sender.has_pending(0, 2));
}

TEST_CASE("end-to-end: a retransmitted packet still decrypts+reassembles correctly on the peer") {
    auto pair = make_connected_pair();
    reliable_sender sender(pair.a);

    std::vector<uint8_t> msg = {7, 7, 7, 7};
    auto packets = sender.send_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 1);

    auto t0 = std::chrono::steady_clock::now();
    sender.collect_retransmits(t0);
    auto retransmits = sender.collect_retransmits(t0 + 250ms);
    REQUIRE(retransmits.size() == 1);

    std::vector<uint8_t> out;
    auto res = pair.b.decrypt_and_reassemble(retransmits[0].data(), retransmits[0].size(), out);
    CHECK(res == transport::result::completed);
    CHECK(out == msg);
}

// =========================================================================
// Regression: malformed SACKs. Found by fuzz_sack_decode - SACK packets are
// the one wire type that is not AEAD-authenticated, so frag_count here is
// attacker-chosen input from anyone who can guess a live conn_id.
// =========================================================================

TEST_CASE("decode_sack rejects a frag_count larger than a 64-bit mask can describe") {
    // frag_count = 53456 is what the fuzzer landed on; it used to reach
    // `1ull << frag_count`, which is undefined behavior.
    uint8_t wire[reliability::sack_wire_len]{};
    wire[0] = 1; // msg_id
    wire[4] = (uint8_t)(53456 & 0xFF);
    wire[5] = (uint8_t)(53456 >> 8);
    for (int i = 0; i < 8; ++i) wire[6 + i] = 0xFF; // all-ones mask

    auto info = reliability::decode_sack(wire);
    CHECK(info.frag_count == 0);   // treated as malformed
    CHECK(info.completed == false); // and definitely not "complete"
}

TEST_CASE("decode_sack still accepts the largest legitimate frag_count") {
    uint8_t wire[reliability::sack_wire_len]{};
    wire[0] = 7;
    wire[4] = (uint8_t)assembly::max_fragments; // exactly 64
    wire[5] = 0;
    for (int i = 0; i < 8; ++i) wire[6 + i] = 0xFF; // all 64 bits set

    auto info = reliability::decode_sack(wire);
    CHECK(info.frag_count == assembly::max_fragments);
    CHECK(info.completed == true); // a full 64-fragment message really is complete
}

TEST_CASE("handle_ack survives an out-of-range frag_count handed to it directly") {
    // handle_ack is a public entry point, so it must bound frag_count itself
    // rather than trusting that decode_sack already did.
    auto pair = make_connected_pair();
    reliability::reliable_sender<> sender(pair.a, std::chrono::milliseconds(20),
                                          std::chrono::milliseconds(200), 5, 4, 64);
    std::vector<uint8_t> msg(100, 0xAB);
    sender.send_message(msg.data(), msg.size());
    size_t pending_before = sender.pending_count();

    assembly::ack_info bogus{};
    bogus.msg_id = 0;
    bogus.frag_count = 65535; // way past what a 64-bit mask can address
    bogus.received_mask = ~0ull;
    bogus.completed = true;
    sender.handle_ack(bogus); // must not invoke UB, must not hang

    // It may legitimately retire the fragments it does cover, but it can
    // never invent new pending state out of a forged ack.
    CHECK(sender.pending_count() <= pending_before);
    CHECK(sender.cwnd() > 0);
}
