#include <doctest/doctest.h>

#include "../net/transport_connection.hpp"

#include <algorithm>
#include <random>

using namespace transport;

namespace {

// Runs a full Noise_IK handshake between a fresh initiator/responder pair
// and returns two Connections wired to each other's session keys, mirroring
// how level4_transport_server/client set one up after level 2's handshake.
struct connection_pair {
    connection<> a; // "client" side
    connection<> b; // "server" side
};

connection_pair make_connected_pair() {
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

    return connection_pair{connection<>(std::move(ik.send), std::move(ik.recv)),
                           connection<>(std::move(rk.send), std::move(rk.recv))};
}

} // namespace

TEST_CASE("single-fragment message: encrypt on one side, decrypt+reassemble on the other") {
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};

    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 1);

    std::vector<uint8_t> out;
    auto res = pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out);
    CHECK(res == transport::result::completed);
    CHECK(out == msg);
}

TEST_CASE("multi-fragment message reassembles correctly through encryption") {
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg(assembly::max_fragment_payload * 3 + 77);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = (uint8_t)i * 3;

    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    CHECK(packets.size() == 4);

    std::vector<uint8_t> out;
    bool completed = false;
    for (auto& pkt : packets) {
        auto res = pair.b.decrypt_and_reassemble(pkt.data(), pkt.size(), out);
        if (res == transport::result::completed) completed = true;
    }
    CHECK(completed);
    CHECK(out == msg);
}

TEST_CASE("out-of-order packet delivery still reassembles byte-exact") {
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg(assembly::max_fragment_payload * 4 + 1);
    std::mt19937 rng(42);
    for (auto& b : msg) b = (uint8_t)rng();

    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    std::reverse(packets.begin(), packets.end());

    std::vector<uint8_t> out;
    bool completed = false;
    for (auto& pkt : packets) {
        auto res = pair.b.decrypt_and_reassemble(pkt.data(), pkt.size(), out);
        if (res == transport::result::completed) completed = true;
    }
    CHECK(completed);
    CHECK(out == msg);
}

TEST_CASE("flipping a ciphertext byte fails authentication, not silent corruption") {
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg = {9, 9, 9};
    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    packets[0].back() ^= 0x01; // last byte is inside the AEAD tag

    std::vector<uint8_t> out;
    auto res = pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out);
    CHECK(res == transport::result::auth_failed);
    CHECK(pair.b.auth_failures() == 1);
}

TEST_CASE("tampering the fragment header (associated data) fails authentication even though the "
          "ciphertext itself is untouched") {
    // This is the property the whole design exists for: msg_id/frag_index/frag_count
    // are plaintext on the wire but authenticated as AEAD associated data, so an
    // attacker who can't forge the tag still can't redirect a fragment into a
    // different message or reorder it by flipping header bits alone.
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg(assembly::max_fragment_payload + 10); // 2 fragments
    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 2);

    // Byte nonce_len+4 is inside frag_index (header layout: msg_id[4] frag_index[2] frag_count[2]).
    packets[0][nonce_len + 4] ^= 0x01;

    std::vector<uint8_t> out;
    auto res = pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out);
    CHECK(res == transport::result::auth_failed);
}

TEST_CASE("decrypting with the wrong connection's keys fails authentication") {
    auto pair_a = make_connected_pair();
    auto pair_b = make_connected_pair();
    std::vector<uint8_t> msg = {1, 2, 3};

    auto packets = pair_a.a.encrypt_message(msg.data(), msg.size());
    std::vector<uint8_t> out;
    auto res = pair_b.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out);
    CHECK(res == transport::result::auth_failed);
}

TEST_CASE("replaying a captured valid packet is rejected before decryption is even attempted") {
    // Level 5's window closes the gap the level-4 tests used to
    // document as a known, deliberately deferred hole: a byte-identical
    // replay of a completed message's packet is now caught at the replay
    // check, before touching the reassembler at all.
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg = {1, 2, 3};
    auto packets = pair.a.encrypt_message(msg.data(), msg.size());

    std::vector<uint8_t> out1;
    CHECK(pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out1) == transport::result::completed);

    std::vector<uint8_t> out2;
    auto replay_result = pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out2);
    CHECK(replay_result == transport::result::replay_rejected);
    CHECK(pair.b.replay_rejections() == 1);
}

TEST_CASE("replaying the first fragment of a still-incomplete message is now rejected, not treated as a "
          "harmless duplicate") {
    // Before level 5, this exact scenario would have been accepted (silently
    // ignored as "just a duplicate fragment," per packet_assembly's own
    // duplicate-fragment behavior) since nothing checked whether a nonce had
    // been consumed before. Replay protection now catches it at the
    // connection layer, one level above the reassembler, before the
    // duplicate-fragment logic even runs.
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg(assembly::max_fragment_payload + 10); // 2 fragments; stays incomplete
    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 2);

    std::vector<uint8_t> out;
    CHECK(pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out) == transport::result::incomplete);

    auto replay_result = pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out);
    CHECK(replay_result == transport::result::replay_rejected);

    // The message can still complete normally via its other, not-yet-seen fragment.
    CHECK(pair.b.decrypt_and_reassemble(packets[1].data(), packets[1].size(), out) == transport::result::completed);
    CHECK(out == msg);
}

TEST_CASE("a forged huge nonce with an invalid ciphertext cannot desync the replay window") {
    // The attack the check()/commit() split specifically defends against:
    // if commit() ran before authentication, an attacker could send a
    // packet with a garbage ciphertext but an enormous nonce to push the
    // window forward and knock every legitimate in-flight nonce out of
    // range - a state-poisoning DoS requiring no forged tag at all.
    auto pair = make_connected_pair();
    std::vector<uint8_t> msg = {1, 2, 3};
    auto legit_packets = pair.a.encrypt_message(msg.data(), msg.size());

    // Craft a forged packet: same wire shape, huge nonce, garbage ciphertext.
    std::vector<uint8_t> forged = legit_packets[0];
    for (size_t i = 0; i < transport::nonce_len; ++i) forged[i] = 0xFF; // nonce = UINT64_MAX
    for (size_t i = transport::data_header_len; i < forged.size(); ++i) forged[i] ^= 0xAA; // corrupt ciphertext

    std::vector<uint8_t> out;
    CHECK(pair.b.decrypt_and_reassemble(forged.data(), forged.size(), out) == transport::result::auth_failed);

    // The legitimate packet, sent *before* the forged one arrived, must still be acceptable.
    CHECK(pair.b.decrypt_and_reassemble(legit_packets[0].data(), legit_packets[0].size(), out) ==
          transport::result::completed);
}

TEST_CASE("keepalive packet authenticates on the receiving side") {
    auto pair = make_connected_pair();
    auto ka = pair.a.encrypt_keepalive();
    CHECK(ka.size() == transport::nonce_len + crypto::aead_tag_len);
    CHECK(pair.b.decrypt_keepalive(ka.data(), ka.size()));
}

TEST_CASE("keepalive never touches the reassembler or produces a completed message") {
    // A keepalive isn't part of any message - decrypt_keepalive must not
    // advance send_msg_id or leave anything for the reassembler to
    // reassemble, so a real message sent right after still comes through
    // as its own single-fragment message, not stitched onto the keepalive.
    auto pair = make_connected_pair();
    auto ka = pair.a.encrypt_keepalive();
    CHECK(pair.b.decrypt_keepalive(ka.data(), ka.size()));

    std::vector<uint8_t> msg = {9, 8, 7};
    auto packets = pair.a.encrypt_message(msg.data(), msg.size());
    REQUIRE(packets.size() == 1);

    std::vector<uint8_t> out;
    CHECK(pair.b.decrypt_and_reassemble(packets[0].data(), packets[0].size(), out) == transport::result::completed);
    CHECK(out == msg);
}

TEST_CASE("a forged/corrupted keepalive fails authentication") {
    auto pair = make_connected_pair();
    auto ka = pair.a.encrypt_keepalive();
    ka.back() ^= 0xFF; // corrupt the AEAD tag
    CHECK_FALSE(pair.b.decrypt_keepalive(ka.data(), ka.size()));
}

TEST_CASE("replaying a captured keepalive is rejected") {
    auto pair = make_connected_pair();
    auto ka = pair.a.encrypt_keepalive();
    CHECK(pair.b.decrypt_keepalive(ka.data(), ka.size()));
    CHECK_FALSE(pair.b.decrypt_keepalive(ka.data(), ka.size()));
}

TEST_CASE("decrypting a keepalive with the wrong connection's keys fails authentication") {
    auto pair_a = make_connected_pair();
    auto pair_b = make_connected_pair();
    auto ka = pair_a.a.encrypt_keepalive();
    CHECK_FALSE(pair_b.b.decrypt_keepalive(ka.data(), ka.size()));
}
