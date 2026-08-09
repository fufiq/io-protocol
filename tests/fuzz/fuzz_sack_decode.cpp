// Fuzzes SACK decoding + the ack path it feeds.
//
// Highest-priority network target, because SACK packets are the one wire
// type that is NOT AEAD-authenticated (see protocol.hpp's tick(): they go
// out through send_wire(5, ...) as plaintext). Anyone who can guess a live
// conn_id can therefore put fully attacker-chosen bytes straight into
// decode_sack() and reliable_sender::handle_ack(). Whatever they do with
// that, it must not corrupt memory, index out of bounds, or wedge the
// sender's pending/timer bookkeeping.
#include "../../net/reliability.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;

    // A real connection pair, so handle_ack() operates against genuine
    // pending state rather than an empty sender that trivially ignores
    // everything.
    static auto ik = crypto::x25519_generate();
    static auto rk = crypto::x25519_generate();
    noise::noise_ik_initiator initiator(ik, rk.pub);
    noise::noise_ik_responder responder(rk);
    auto m1 = initiator.write_message1(nullptr, 0);
    responder.read_message1(m1);
    auto m2 = responder.write_message2(nullptr, 0);
    initiator.read_message2(m2);
    auto ikeys = initiator.finish();

    transport::connection<> conn(std::move(ikeys.send), std::move(ikeys.recv));
    reliability::reliable_sender<> sender(conn, std::chrono::milliseconds(20),
                                          std::chrono::milliseconds(200), 5, 4, 64);

    // Put some real fragments in flight to be acked (or mis-acked).
    std::vector<uint8_t> payload(200, 0xAB);
    for (int i = 0; i < 3; ++i) sender.send_message(payload.data(), payload.size());
    const size_t pending_before = sender.pending_count();

    // Mirror protocol.hpp's handle_sack() parsing exactly: count byte, then
    // that many fixed-width entries, bounded by what actually arrived.
    uint8_t count = data[0];
    for (uint8_t i = 0; i < count; ++i) {
        size_t off = 1 + (size_t)i * reliability::sack_wire_len;
        if (off + reliability::sack_wire_len > size) break;
        sender.handle_ack(reliability::decode_sack(data + off));
    }

    // An ack can only ever retire pending fragments, never invent new ones:
    // a forged SACK that grew the pending set would mean the attacker can
    // drive unbounded memory/timer growth from off-path.
    if (sender.pending_count() > pending_before) __builtin_trap();

    // The congestion window must stay within its own documented bounds no
    // matter what was fed in - a forged ack that inflates cwnd without
    // limit is a remote amplification lever.
    if (sender.cwnd() == 0) __builtin_trap();
    if (sender.cwnd() > (1u << 20)) __builtin_trap();

    // Retransmit collection must stay well-behaved afterwards.
    auto rts = sender.collect_retransmits(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    if (rts.size() > pending_before) __builtin_trap();
    return 0;
}
