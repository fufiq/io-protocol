#pragma once

// Level 4: ties the Noise_IK handshake (level 2) and fragment reassembly
// (level 3) into one coherent post-handshake session.
//
// Wire format per packet (this is everything after whatever outer
// type/conn_id framing a caller uses to demux which connection a packet
// belongs to - see level4_transport_server.cpp):
//
//   nonce(8 LE) | fragment_header(8: msg_id,frag_index,frag_count) | ciphertext(payload_len+16)
//
// The critical decision: fragment_header is passed to the AEAD call as
// *associated data*, not encrypted. It doesn't need confidentiality (an
// observer already sees packet sizes and timing), but it absolutely needs
// integrity - packet_assembly::reassembler trusts msg_id/frag_index/
// frag_count completely (see packet_assembly.hpp's own layering note: "this
// code trusts every fragment handed to it"). If the header were plaintext
// with no authentication, an attacker who can't forge the AEAD tag could
// still flip frag_index bits or corrupt msg_id on a genuine packet in
// transit, silently corrupting reassembly for a victim who never sees an
// auth failure. Binding it as AD means any tamper - to the header OR the
// ciphertext - fails the same AEAD check, before the reassembler ever sees
// it. This is the actual meaning of "decrypt-then-reassemble" here: it's
// not just an ordering convention, it's what makes the reassembly's
// trust-everything-it's-handed design (chosen deliberately in level 3 for
// simplicity) safe to expose to a network at all.
//
// Replay protection (level 5): the nonce is strictly increasing on the
// *send* side (required - AEAD nonce reuse under the same key is a real
// key-recovery-adjacent failure, not just a bug) and checked against a
// replay::window on the *receive* side before decryption is even
// attempted. The check/commit split matters: check() runs on the
// unauthenticated nonce and must not mutate window state (an attacker could
// otherwise push the window forward with a forged huge nonce and desync
// legitimate traffic out of it "for free"); commit() only runs after AEAD
// verification succeeds. See replay_window.hpp for the full reasoning.

#include "noise_ik.hpp"
#include "packet_assembly.hpp"
#include "replay_window.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace transport {

constexpr size_t nonce_len = 8;
constexpr size_t data_header_len = nonce_len + assembly::fragment_header_len;

// Free enum at namespace scope, not nested in connection<window_capacity>:
// its meaning doesn't depend on the window size, so callers can name it as
// plain transport::result regardless of which connection<N> they're
// talking to (see assembly::result/assembly::ack_info in packet_assembly.hpp
// for the same reasoning).
enum class result {
    replay_rejected,       // nonce outside the window or already seen; rejected before decryption was attempted
    auth_failed,           // AEAD tag didn't verify: forged/corrupted/wrong-key packet
    reassembly_rejected,   // authenticated, but packet_assembly::reassembler rejected it (see its own reasons)
    incomplete,           // authenticated and buffered; message isn't complete yet
    completed,            // authenticated and this was the last missing fragment; out_message is ready
    duplicate_of_completed, // authenticated retransmission of a message already fully received - see above
};

// window_capacity is a template parameter for the same reason
// reassembler's own window_capacity is (see packet_assembly.hpp): every real
// caller uses the same 4096-slot window, so the one non-default-able
// requirement (power of two) is a compile-time static_assert instead of a
// runtime throw.
template <size_t window_capacity = 4096>
class connection {
public:
    connection(noise::cipher_state send_key, noise::cipher_state recv_key)
        : send_key(std::move(send_key)), recv_key(std::move(recv_key)) {}

    // Fragments `data` if needed (assembly::fragment_message) and
    // AEAD-encrypts each resulting fragment independently, one AEAD op per
    // fragment with its own fresh nonce. Returns complete wire-ready
    // packets in `nonce|header|ciphertext` form.
    std::vector<std::vector<uint8_t>> encrypt_message(const uint8_t* data, size_t len) {
        auto plaintext_fragments = fragment_for_send(data, len);

        std::vector<std::vector<uint8_t>> out;
        out.reserve(plaintext_fragments.size());

        for (auto& frag : plaintext_fragments) {
            auto hdr = assembly::decode_header(frag.data());
            const uint8_t* payload = frag.data() + assembly::fragment_header_len;
            size_t payload_len = frag.size() - assembly::fragment_header_len;
            out.push_back(encrypt_fragment(hdr, payload, payload_len));
        }
        return out;
    }

    // Splits `data` into plaintext fragments (msg_id + header + payload,
    // unencrypted - assembly::fragment_message's own output format) using
    // this connection's msg_id sequence, WITHOUT encrypting them. Building
    // block for reliability::reliable_sender, which must retain the
    // plaintext (not just a finished ciphertext) so it can re-encrypt a
    // retransmission with a fresh nonce via encrypt_fragment() below,
    // rather than resending the exact old bytes - see encrypt_fragment's
    // own comment for why that distinction is load-bearing, not stylistic.
    std::vector<std::vector<uint8_t>> fragment_for_send(const uint8_t* data, size_t len) {
        uint32_t msg_id = send_msg_id++;
        return assembly::fragment_message(msg_id, data, len);
    }

    // Encrypts one already-chunked fragment (header + plaintext) with the
    // *next* nonce from this connection's counter - the single building
    // block encrypt_message() uses internally for each chunk, and the one
    // reliability::reliable_sender calls again, with the same msg_id/
    // frag_index/frag_count but a fresh nonce, to retransmit a fragment
    // whose ack never arrived.
    //
    // Retransmission must never resend a previously-used nonce's exact
    // ciphertext: replay::window (level 5) exists precisely to reject
    // a byte-identical repeat of anything it's already committed, and it
    // cannot distinguish "this is an attacker replaying a captured packet"
    // from "this is the legitimate sender retrying because my ack got
    // lost" - both look identical on the wire. A fresh nonce sidesteps the
    // question entirely: the replay window sees a nonce it's never seen,
    // authenticates normally, and it's packet_assembly::reassembler's
    // result::duplicate_of_completed (not the replay window) that recognizes
    // "I already have this message" and re-triggers a SACK so the sender
    // can learn that and stop retrying.
    std::vector<uint8_t> encrypt_fragment(const assembly::fragment_header& hdr, const uint8_t* plaintext, size_t plaintext_len) {
        uint8_t header_bytes[assembly::fragment_header_len];
        assembly::encode_header(hdr, header_bytes);

        uint64_t nonce = send_nonce++;
        auto ciphertext = crypto::aead_encrypt(send_key.key(), nonce, header_bytes, assembly::fragment_header_len, plaintext, plaintext_len);

        std::vector<uint8_t> wire(data_header_len + ciphertext.size());
        for (int i = 0; i < 8; ++i) wire[i] = (uint8_t)(nonce >> (8 * i));
        std::memcpy(wire.data() + nonce_len, header_bytes, assembly::fragment_header_len);
        std::memcpy(wire.data() + data_header_len, ciphertext.data(), ciphertext.size());
        return wire;
    }

    // Convenience overload for callers that don't need the completed
    // message's msg_id (every pre-existing caller) - see the msg_id
    // overload below for why it exists and who actually needs it.
    result decrypt_and_reassemble(const uint8_t* wire, size_t len, std::vector<uint8_t>& out_message) {
        uint32_t discard_msg_id;
        return decrypt_and_reassemble(wire, len, out_message, discard_msg_id);
    }

    // Decrypts+authenticates one inbound wire packet, then feeds the
    // recovered (msg_id, frag_index, frag_count, plaintext-fragment-bytes)
    // into this connection's reassembler. out_msg_id is set to this
    // packet's fragment header msg_id whenever decryption succeeds far
    // enough to decode a header at all (regardless of the reassembly
    // outcome) - for a completed message every one of its fragments shares
    // the same msg_id, so this is unambiguously *the* message's id, not
    // just this one fragment's. Needed by callers that must apply
    // completed messages in msg_id order (replication is the reason this
    // exists - see replication.hpp - reassembly here already handles
    // within-message fragment reordering, but says nothing about whether
    // message 5 might complete before message 4 does).
    result decrypt_and_reassemble(const uint8_t* wire, size_t len, std::vector<uint8_t>& out_message, uint32_t& out_msg_id) {
        if (len < data_header_len + crypto::aead_tag_len) {
            ++authentication_failure_count;
            return result::auth_failed;
        }

        uint64_t nonce = 0;
        for (int i = 0; i < 8; ++i) nonce |= (uint64_t)wire[i] << (8 * i);
        const uint8_t* header_bytes = wire + nonce_len;
        const uint8_t* ciphertext = wire + data_header_len;
        size_t ciphertext_len = len - data_header_len;

        if (!replay_window.check(nonce)) {
            ++replay_rejection_count;
            return result::replay_rejected;
        }

        auto plaintext = crypto::aead_decrypt(recv_key.key(), nonce, header_bytes, assembly::fragment_header_len, ciphertext, ciphertext_len);
        if (!plaintext) {
            ++authentication_failure_count;
            return result::auth_failed;
        }
        replay_window.commit(nonce); // only after authentication succeeds - see the class-level comment on why

        auto hdr = assembly::decode_header(header_bytes);
        out_msg_id = hdr.msg_id;
        auto res = reassembly.add_fragment(hdr, plaintext->data(), plaintext->size(), out_message);
        switch (res) {
            case assembly::result::completed: return result::completed;
            case assembly::result::incomplete: return result::incomplete;
            case assembly::result::rejected: return result::reassembly_rejected;
            case assembly::result::duplicate_of_completed: return result::duplicate_of_completed;
        }
        return result::reassembly_rejected;
    }

    // NAT keepalive (see protocol.hpp's idle-timer comment for why this
    // exists at all): a minimal authenticated packet with no msg_id/
    // frag_index concept at all, just nonce + AEAD(empty plaintext, no
    // associated data). Shares the same send_nonce counter as real data -
    // a nonce only has to be unique per connection direction, it doesn't
    // matter what kind of packet it was spent on. Deliberately bypasses
    // fragment_for_send/the reassembler entirely: a keepalive isn't part
    // of any message, so there's nothing for the receiver to reassemble
    // and no reason to burn a msg_id on it.
    std::vector<uint8_t> encrypt_keepalive() {
        uint64_t nonce = send_nonce++;
        auto ciphertext = crypto::aead_encrypt(send_key.key(), nonce, nullptr, 0, nullptr, 0);
        std::vector<uint8_t> wire(nonce_len + ciphertext.size());
        for (int i = 0; i < 8; ++i) wire[i] = (uint8_t)(nonce >> (8 * i));
        std::memcpy(wire.data() + nonce_len, ciphertext.data(), ciphertext.size());
        return wire;
    }

    // Authenticates an inbound keepalive packet (replay-checked exactly
    // like a data packet's nonce, just without any reassembly step
    // afterward). Returns false on a replayed/forged/malformed packet -
    // callers should treat that the same as any other authentication
    // failure (count it, don't act on it), not as a protocol error worth
    // surfacing to the application.
    bool decrypt_keepalive(const uint8_t* wire, size_t len) {
        if (len < nonce_len + crypto::aead_tag_len) {
            ++authentication_failure_count;
            return false;
        }

        uint64_t nonce = 0;
        for (int i = 0; i < 8; ++i) nonce |= (uint64_t)wire[i] << (8 * i);
        const uint8_t* ciphertext = wire + nonce_len;
        size_t ciphertext_len = len - nonce_len;

        if (!replay_window.check(nonce)) {
            ++replay_rejection_count;
            return false;
        }

        auto plaintext = crypto::aead_decrypt(recv_key.key(), nonce, nullptr, 0, ciphertext, ciphertext_len);
        if (!plaintext) {
            ++authentication_failure_count;
            return false;
        }
        replay_window.commit(nonce);
        return true;
    }

    uint64_t auth_failures() const { return authentication_failure_count; }
    uint64_t replay_rejections() const { return replay_rejection_count; }
    size_t bytes_buffered() const { return reassembly.bytes_buffered(); }

    // Level 6 hook: passes through to the underlying reassembler's own
    // dirty-tracking (see packet_assembly.hpp) so a SACK-generating caller
    // doesn't need its own parallel bookkeeping of "what have I received."
    std::vector<assembly::ack_info> drain_dirty_acks() { return reassembly.drain_dirty_acks(); }

private:
    noise::cipher_state send_key;
    noise::cipher_state recv_key;
    uint64_t send_nonce = 0;
    uint32_t send_msg_id = 0;
    assembly::reassembler<window_capacity> reassembly;
    replay::window replay_window;
    uint64_t authentication_failure_count = 0;
    uint64_t replay_rejection_count = 0;
};

} // namespace transport
