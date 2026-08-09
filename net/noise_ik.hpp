#pragma once

// Noise_IK_25519_ChaChaPoly_SHA256 handshake, hardcoded to the IK pattern
// rather than built as a generic Noise token interpreter: a fixed state
// machine is auditable end-to-end and has no room for a pattern-interpreter
// bug to smuggle in a token sequence nobody reviewed.
//
//   <- s
//   ...
//   -> e, es, s, ss
//   <- e, ee, se
//
// The initiator must already know the responder's static public key
// out-of-band (that is what "IK" means: Initiator knows, static Key
// pre-shared). This hides the initiator's identity (their static key is
// AEAD-encrypted in message 1) from passive eavesdroppers; the responder's
// identity is never sent on the wire at all.
//
// Wire framing adds one thing on top of bare Noise: a 16-byte "mac1" tag
// (HMAC-SHA256 keyed on a hash of the responder's static public key,
// truncated to 16 bytes) appended to both messages, WireGuard-style. This
// is NOT part of the Noise handshake proper - it's a cheap pre-filter so a
// responder can reject packets from senders who don't even know its public
// key for the cost of one HMAC, before spending a DH on them. It does NOT
// stop replay of a captured valid packet (mac1 is deterministic over
// content the attacker already has), and it does NOT stop a flood from an
// attacker who *does* know the public key - that needs the stateless
// cookie/mac2 mechanism, deferred to level 5.
//
// Doctest (tests/test_noise_ik.cpp) covers: full handshake round trip with
// matching split transport keys, tamper detection on every AEAD'd field,
// mac1 rejection of garbage before any DH, and DH-invalid-point rejection.

#include "crypto_primitives.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace noise {

namespace detail {
inline constexpr char protocol_name[] = "Noise_IK_25519_ChaChaPoly_SHA256";
static_assert(sizeof(protocol_name) - 1 == 32, "protocol_name must be exactly 32 bytes (excluding the null terminator)");
inline constexpr char mac1_label[] = "mac1----"; // 8 bytes, WireGuard-style fixed-length label

inline crypto::hash mac1_key(const crypto::public_key& responder_static_pub) {
    std::vector<uint8_t> buf(mac1_label, mac1_label + 8);
    buf.insert(buf.end(), responder_static_pub.begin(), responder_static_pub.end());
    return crypto::sha256(buf.data(), buf.size());
}

inline void append_mac1(std::vector<uint8_t>& msg, const crypto::public_key& responder_static_pub) {
    auto key = mac1_key(responder_static_pub);
    auto mac = crypto::mac16(key, msg.data(), msg.size());
    msg.insert(msg.end(), mac.begin(), mac.end());
}
} // namespace detail

// AEAD wrapper bound to the Noise nonce-counter convention. Not reused for
// the level-4 transport cipher: post-handshake transport traffic needs a
// sliding replay window instead of a strictly-incrementing counter (packets
// arrive out of order over UDP), which is a distinct enough contract that
// conflating the two here would be the wrong abstraction.
class cipher_state {
public:
    cipher_state() : key_bytes(crypto::key_len) {}

    void init_key(const crypto::hash& key) {
        std::memcpy(key_bytes.data(), key.data(), key.size());
        key_is_set = true;
        nonce_counter = 0;
    }

    bool has_key() const { return key_is_set; }
    uint64_t nonce() const { return nonce_counter; }

    // Escape hatch for callers that need an *explicit* per-packet nonce
    // instead of this class's own strictly-incrementing internal counter -
    // e.g. a UDP transport where packets can arrive out of order or be
    // lost, so the receiver cannot assume "the Nth decrypt uses nonce N".
    // encrypt_with_ad/decrypt_with_ad are correct only for the in-order,
    // exactly-once exchange inside a single handshake; anything with
    // reordering/loss must pull the raw key out via this accessor and drive
    // crypto::aead_encrypt/aead_decrypt directly with a nonce carried on
    // the wire. See level2_post_handshake_pps_{server,client}.cpp.
    const crypto::secret_buffer& key() const { return key_bytes; }

    std::vector<uint8_t> encrypt_with_ad(const uint8_t* ad, size_t ad_len, const uint8_t* pt, size_t pt_len) {
        if (!key_is_set) return std::vector<uint8_t>(pt, pt + pt_len);
        auto ct = crypto::aead_encrypt(key_bytes, nonce_counter, ad, ad_len, pt, pt_len);
        ++nonce_counter;
        return ct;
    }

    std::optional<std::vector<uint8_t>> decrypt_with_ad(const uint8_t* ad, size_t ad_len, const uint8_t* ct, size_t ct_len) {
        if (!key_is_set) return std::vector<uint8_t>(ct, ct + ct_len);
        auto pt = crypto::aead_decrypt(key_bytes, nonce_counter, ad, ad_len, ct, ct_len);
        if (!pt) return std::nullopt; // nonce NOT advanced on failure: a failed cipher_state is dead, never retried
        ++nonce_counter;
        return pt;
    }

private:
    crypto::secret_buffer key_bytes;
    bool key_is_set = false;
    uint64_t nonce_counter = 0;
};

// Noise symmetric_state: running handshake hash `h` (transcript binding) and
// chaining key `ck` (KDF chain), plus the cipher_state used for
// EncryptAndHash/DecryptAndHash of handshake payloads. Generic across any
// Noise pattern; the IK-specific token sequencing lives in
// noise_ik_initiator/noise_ik_responder below, not here.
class symmetric_state {
public:
    symmetric_state(const char* protocol_name, size_t name_len) {
        if (name_len <= crypto::hash_len) {
            h.fill(0);
            std::memcpy(h.data(), protocol_name, name_len);
        } else {
            h = crypto::sha256((const uint8_t*)protocol_name, name_len);
        }
        ck = h;
    }

    void mix_hash(const uint8_t* data, size_t len) {
        std::vector<uint8_t> buf(h.begin(), h.end());
        if (len > 0) buf.insert(buf.end(), data, data + len);
        h = crypto::sha256(buf.data(), buf.size());
    }

    void mix_key(const uint8_t* ikm, size_t ikm_len) {
        crypto::hash new_ck{};
        crypto::hash temp_k{};
        crypto::hkdf2(ck, ikm, ikm_len, new_ck, temp_k);
        ck = new_ck;
        cipher.init_key(temp_k);
    }

    std::vector<uint8_t> encrypt_and_hash(const uint8_t* pt, size_t pt_len) {
        auto ct = cipher.encrypt_with_ad(h.data(), h.size(), pt, pt_len);
        mix_hash(ct.data(), ct.size());
        return ct;
    }

    std::optional<std::vector<uint8_t>> decrypt_and_hash(const uint8_t* ct, size_t ct_len) {
        auto pt = cipher.decrypt_with_ad(h.data(), h.size(), ct, ct_len);
        if (!pt) return std::nullopt;
        mix_hash(ct, ct_len);
        return pt;
    }

    crypto::hash h{};
    crypto::hash ck{};
    cipher_state cipher;
};

struct transport_keys {
    cipher_state send;
    cipher_state recv;
};

inline std::pair<cipher_state, cipher_state> split(symmetric_state& state) {
    crypto::hash k1{};
    crypto::hash k2{};
    crypto::hkdf2(state.ck, nullptr, 0, k1, k2);
    cipher_state c1;
    cipher_state c2;
    c1.init_key(k1);
    c2.init_key(k2);
    return {std::move(c1), std::move(c2)};
}

class handshake_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class noise_ik_initiator {
public:
    noise_ik_initiator(const crypto::key_pair& local_static, const crypto::public_key& remote_static)
        : handshake_state(detail::protocol_name, sizeof(detail::protocol_name) - 1), remote_static_key(remote_static) {
        std::memcpy(local_static_keypair.priv.data(), local_static.priv.data(), crypto::key_len);
        local_static_keypair.pub = local_static.pub;

        handshake_state.mix_hash(nullptr, 0);                                       // empty prologue
        handshake_state.mix_hash(remote_static_key.data(), remote_static_key.size()); // pre-message "<- s": responder's static key, known out-of-band
    }

    // Produces message 1 on the wire: ephemeral(32) || enc_static(48) || enc_payload(len+16) || mac1(16).
    std::vector<uint8_t> write_message1(const uint8_t* payload, size_t payload_len) {
        ephemeral_keypair = crypto::x25519_generate();
        handshake_state.mix_hash(ephemeral_keypair.pub.data(), ephemeral_keypair.pub.size());

        auto es = crypto::dh(ephemeral_keypair.priv, remote_static_key);
        if (!es) throw handshake_error("write_message1: dh(e, rs) rejected (invalid remote static key)");
        handshake_state.mix_key(es->data(), es->size());

        auto enc_static = handshake_state.encrypt_and_hash(local_static_keypair.pub.data(), local_static_keypair.pub.size());

        auto ss_dh = crypto::dh(local_static_keypair.priv, remote_static_key);
        if (!ss_dh) throw handshake_error("write_message1: dh(s, rs) rejected");
        handshake_state.mix_key(ss_dh->data(), ss_dh->size());

        auto enc_payload = handshake_state.encrypt_and_hash(payload, payload_len);

        std::vector<uint8_t> msg;
        msg.reserve(ephemeral_keypair.pub.size() + enc_static.size() + enc_payload.size() + crypto::mac_len);
        msg.insert(msg.end(), ephemeral_keypair.pub.begin(), ephemeral_keypair.pub.end());
        msg.insert(msg.end(), enc_static.begin(), enc_static.end());
        msg.insert(msg.end(), enc_payload.begin(), enc_payload.end());
        detail::append_mac1(msg, remote_static_key);
        return msg;
    }

    // Consumes message 2, returns the decrypted payload. Throws handshake_error on any failure
    // (bad mac1, bad DH input, auth failure) - a failed handshake attempt is always fatal to
    // this noise_ik_initiator instance, it is never resumed.
    std::vector<uint8_t> read_message2(const std::vector<uint8_t>& msg);

    transport_keys finish() {
        auto [c1, c2] = split(handshake_state);
        return {std::move(c1), std::move(c2)}; // initiator: send=c1, recv=c2
    }

private:
    symmetric_state handshake_state;
    crypto::key_pair local_static_keypair;
    crypto::public_key remote_static_key;
    crypto::key_pair ephemeral_keypair;
};

class noise_ik_responder {
public:
    explicit noise_ik_responder(const crypto::key_pair& local_static) : handshake_state(detail::protocol_name, sizeof(detail::protocol_name) - 1) {
        std::memcpy(local_static_keypair.priv.data(), local_static.priv.data(), crypto::key_len);
        local_static_keypair.pub = local_static.pub;

        handshake_state.mix_hash(nullptr, 0);
        handshake_state.mix_hash(local_static_keypair.pub.data(), local_static_keypair.pub.size()); // pre-message "<- s": our own static key
    }

    // Stateless: verifies mac1 without allocating a HandshakeState or touching any DH.
    // Callers should run this against raw inbound bytes before deciding whether a packet
    // is even worth constructing a noise_ik_responder for.
    static bool verify_mac1(const crypto::public_key& local_static_pub, const uint8_t* msg, size_t msg_len) {
        if (msg_len < crypto::mac_len) return false;
        size_t body_len = msg_len - crypto::mac_len;
        auto key = detail::mac1_key(local_static_pub);
        auto expected = crypto::mac16(key, msg, body_len);
        return crypto::constant_time_eq(expected.data(), msg + body_len, crypto::mac_len);
    }

    struct message1_result {
        crypto::public_key initiator_static;
        std::vector<uint8_t> payload;
    };

    message1_result read_message1(const std::vector<uint8_t>& msg);
    std::vector<uint8_t> write_message2(const uint8_t* payload, size_t payload_len);

    transport_keys finish() {
        auto [c1, c2] = split(handshake_state);
        return {std::move(c2), std::move(c1)}; // responder: send=c2, recv=c1 (mirrors initiator's c1/c2)
    }

private:
    symmetric_state handshake_state;
    crypto::key_pair local_static_keypair;
    crypto::key_pair ephemeral_keypair;
    crypto::public_key remote_ephemeral_key{};
    crypto::public_key initiator_static{};
};

inline std::vector<uint8_t> noise_ik_initiator::read_message2(const std::vector<uint8_t>& msg) {
    if (!noise_ik_responder::verify_mac1(remote_static_key, msg.data(), msg.size())) throw handshake_error("read_message2: mac1 mismatch");
    size_t body_len = msg.size() - crypto::mac_len;
    if (body_len < crypto::key_len + crypto::aead_tag_len) throw handshake_error("read_message2: message too short");

    crypto::public_key re{};
    std::memcpy(re.data(), msg.data(), crypto::key_len);
    handshake_state.mix_hash(re.data(), re.size());

    auto ee = crypto::dh(ephemeral_keypair.priv, re);
    if (!ee) throw handshake_error("read_message2: dh(e, re) rejected");
    handshake_state.mix_key(ee->data(), ee->size());

    auto se = crypto::dh(local_static_keypair.priv, re);
    if (!se) throw handshake_error("read_message2: dh(s, re) rejected");
    handshake_state.mix_key(se->data(), se->size());

    size_t payload_ct_len = body_len - crypto::key_len;
    auto payload = handshake_state.decrypt_and_hash(msg.data() + crypto::key_len, payload_ct_len);
    if (!payload) throw handshake_error("read_message2: payload authentication failed");
    return *payload;
}

inline noise_ik_responder::message1_result noise_ik_responder::read_message1(const std::vector<uint8_t>& msg) {
    if (!verify_mac1(local_static_keypair.pub, msg.data(), msg.size())) throw handshake_error("read_message1: mac1 mismatch");
    size_t body_len = msg.size() - crypto::mac_len;
    constexpr size_t enc_static_len = crypto::key_len + crypto::aead_tag_len;
    if (body_len < crypto::key_len + enc_static_len) throw handshake_error("read_message1: message too short");

    std::memcpy(remote_ephemeral_key.data(), msg.data(), crypto::key_len);
    handshake_state.mix_hash(remote_ephemeral_key.data(), remote_ephemeral_key.size());

    auto es = crypto::dh(local_static_keypair.priv, remote_ephemeral_key); // "es" = DH(e_i, s_r): responder computes DH(s_r_priv, e_i_pub)
    if (!es) throw handshake_error("read_message1: dh(s, re) rejected");
    handshake_state.mix_key(es->data(), es->size());

    auto static_pt = handshake_state.decrypt_and_hash(msg.data() + crypto::key_len, enc_static_len);
    if (!static_pt) throw handshake_error("read_message1: static key authentication failed");
    std::memcpy(initiator_static.data(), static_pt->data(), crypto::key_len);

    auto ss_dh = crypto::dh(local_static_keypair.priv, initiator_static); // "ss"
    if (!ss_dh) throw handshake_error("read_message1: dh(s, rs_initiator) rejected");
    handshake_state.mix_key(ss_dh->data(), ss_dh->size());

    size_t off = crypto::key_len + enc_static_len;
    size_t payload_ct_len = body_len - off;
    auto payload = handshake_state.decrypt_and_hash(msg.data() + off, payload_ct_len);
    if (!payload) throw handshake_error("read_message1: payload authentication failed");

    return {initiator_static, *payload};
}

inline std::vector<uint8_t> noise_ik_responder::write_message2(const uint8_t* payload, size_t payload_len) {
    ephemeral_keypair = crypto::x25519_generate();
    handshake_state.mix_hash(ephemeral_keypair.pub.data(), ephemeral_keypair.pub.size());

    auto ee = crypto::dh(ephemeral_keypair.priv, remote_ephemeral_key);
    if (!ee) throw handshake_error("write_message2: dh(e, re) rejected");
    handshake_state.mix_key(ee->data(), ee->size());

    auto se = crypto::dh(ephemeral_keypair.priv, initiator_static); // "se" = DH(s_i, e_r): responder computes DH(e_r_priv, s_i_pub)
    if (!se) throw handshake_error("write_message2: dh(e, rs_initiator) rejected");
    handshake_state.mix_key(se->data(), se->size());

    auto enc_payload = handshake_state.encrypt_and_hash(payload, payload_len);

    std::vector<uint8_t> msg;
    msg.reserve(ephemeral_keypair.pub.size() + enc_payload.size() + crypto::mac_len);
    msg.insert(msg.end(), ephemeral_keypair.pub.begin(), ephemeral_keypair.pub.end());
    msg.insert(msg.end(), enc_payload.begin(), enc_payload.end());
    detail::append_mac1(msg, local_static_keypair.pub);
    return msg;
}

} // namespace noise
