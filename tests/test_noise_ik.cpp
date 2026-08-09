#include <doctest/doctest.h>

#include "../net/noise_ik.hpp"

using namespace noise;

namespace {

struct peers_bundle {
    crypto::key_pair initiator_static = crypto::x25519_generate();
    crypto::key_pair responder_static = crypto::x25519_generate();
};

} // namespace

TEST_CASE("full IK handshake: both sides derive matching, cross-wired transport keys") {
    peers_bundle p;
    noise_ik_initiator initiator(p.initiator_static, p.responder_static.pub);
    noise_ik_responder responder(p.responder_static);

    const uint8_t payload1[] = "hello from initiator";
    auto msg1 = initiator.write_message1(payload1, sizeof(payload1));

    auto r1 = responder.read_message1(msg1);
    CHECK(std::memcmp(r1.initiator_static.data(), p.initiator_static.pub.data(), crypto::key_len) == 0);
    CHECK(std::memcmp(r1.payload.data(), payload1, sizeof(payload1)) == 0);

    const uint8_t payload2[] = "hello from responder";
    auto msg2 = responder.write_message2(payload2, sizeof(payload2));

    auto out_payload2 = initiator.read_message2(msg2);
    CHECK(std::memcmp(out_payload2.data(), payload2, sizeof(payload2)) == 0);

    transport_keys ik = initiator.finish();
    transport_keys rk = responder.finish();

    // Initiator's send key must decrypt under responder's recv key, and vice versa.
    const uint8_t app_msg[] = "post-handshake application data";
    auto ct = ik.send.encrypt_with_ad(nullptr, 0, app_msg, sizeof(app_msg));
    auto pt = rk.recv.decrypt_with_ad(nullptr, 0, ct.data(), ct.size());
    REQUIRE(pt.has_value());
    CHECK(std::memcmp(pt->data(), app_msg, sizeof(app_msg)) == 0);

    auto ct2 = rk.send.encrypt_with_ad(nullptr, 0, app_msg, sizeof(app_msg));
    auto pt2 = ik.recv.decrypt_with_ad(nullptr, 0, ct2.data(), ct2.size());
    REQUIRE(pt2.has_value());
    CHECK(std::memcmp(pt2->data(), app_msg, sizeof(app_msg)) == 0);
}

TEST_CASE("responder rejects message1 when initiator used the wrong responder static key") {
    peers_bundle p;
    auto wrong_responder = crypto::x25519_generate();

    noise_ik_initiator initiator(p.initiator_static, wrong_responder.pub); // initiator thinks this is the responder
    noise_ik_responder responder(p.responder_static);                     // but this is the real responder

    auto msg1 = initiator.write_message1(nullptr, 0);

    // mac1 itself was computed against wrong_responder's key, not the real
    // responder's, so this must be rejected at the mac1 stage already.
    CHECK_THROWS_AS(responder.read_message1(msg1), handshake_error);
}

TEST_CASE("mac1 rejects a random garbage packet before any DH is attempted") {
    peers_bundle p;
    noise_ik_responder responder(p.responder_static);

    std::vector<uint8_t> garbage(96);
    crypto::random_bytes(garbage.data(), garbage.size());

    CHECK_FALSE(noise_ik_responder::verify_mac1(p.responder_static.pub, garbage.data(), garbage.size()));
    CHECK_THROWS_AS(responder.read_message1(garbage), handshake_error);
}

TEST_CASE("flipping any byte in message1's encrypted static key field breaks the handshake") {
    peers_bundle p;
    noise_ik_initiator initiator(p.initiator_static, p.responder_static.pub);
    auto msg1 = initiator.write_message1(nullptr, 0);

    // Byte 40 sits inside the AEAD-encrypted static-key field (offset 32..80).
    msg1[40] ^= 0x01;
    // Recompute mac1 so the tamper is caught by the AEAD tag, not the cheap mac1 pre-filter -
    // this proves the *Noise* authentication is doing its job, not just the DoS pre-filter.
    msg1.resize(msg1.size() - crypto::mac_len);
    std::vector<uint8_t> label_and_pub((const uint8_t*)"mac1----",
                                        (const uint8_t*)"mac1----" + 8);
    label_and_pub.insert(label_and_pub.end(), p.responder_static.pub.begin(), p.responder_static.pub.end());
    auto mac1_key = crypto::sha256(label_and_pub.data(), label_and_pub.size());
    auto mac = crypto::mac16(mac1_key, msg1.data(), msg1.size());
    msg1.insert(msg1.end(), mac.begin(), mac.end());

    noise_ik_responder responder(p.responder_static);
    CHECK_THROWS_AS(responder.read_message1(msg1), handshake_error);
}

TEST_CASE("flipping the mac1 tag alone is rejected without touching the AEAD layer") {
    peers_bundle p;
    noise_ik_initiator initiator(p.initiator_static, p.responder_static.pub);
    auto msg1 = initiator.write_message1(nullptr, 0);
    msg1.back() ^= 0x01; // last byte is inside mac1

    noise_ik_responder responder(p.responder_static);
    CHECK_THROWS_AS(responder.read_message1(msg1), handshake_error);
}

TEST_CASE("initiator rejects a tampered message2") {
    peers_bundle p;
    noise_ik_initiator initiator(p.initiator_static, p.responder_static.pub);
    noise_ik_responder responder(p.responder_static);

    auto msg1 = initiator.write_message1(nullptr, 0);
    responder.read_message1(msg1);
    auto msg2 = responder.write_message2(nullptr, 0);

    msg2[0] ^= 0x01; // corrupt responder's ephemeral public key
    CHECK_THROWS_AS(initiator.read_message2(msg2), handshake_error);
}

TEST_CASE("a second, independent handshake between the same two static keys derives different transport keys") {
    // Guards against an implementation accidentally deriving session keys
    // from long-term static keys alone (which would make every session
    // between the same two peers use identical traffic keys - a
    // forward-secrecy failure). Freshness must come from the ephemerals.
    peers_bundle p;

    auto run_handshake = [&]() -> transport_keys {
        noise_ik_initiator initiator(p.initiator_static, p.responder_static.pub);
        noise_ik_responder responder(p.responder_static);
        auto msg1 = initiator.write_message1(nullptr, 0);
        responder.read_message1(msg1);
        auto msg2 = responder.write_message2(nullptr, 0);
        initiator.read_message2(msg2);
        return initiator.finish();
    };

    transport_keys k1 = run_handshake();
    transport_keys k2 = run_handshake();

    const uint8_t msg[] = "probe";
    auto ct1 = k1.send.encrypt_with_ad(nullptr, 0, msg, sizeof(msg));
    auto ct2 = k2.send.encrypt_with_ad(nullptr, 0, msg, sizeof(msg));
    CHECK(std::memcmp(ct1.data(), ct2.data(), std::min(ct1.size(), ct2.size())) != 0);
}
