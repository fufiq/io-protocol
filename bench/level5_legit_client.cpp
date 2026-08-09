// Level 5 demonstration, well-behaved client: performs real handshakes
// against level5_anti_dos_server, transparently handling a cookie-reply
// (type 4) by resending the *same* message1 with mac2 attached - mac2 is
// computed over the message1 bytes, which don't change between the two
// attempts, so there's no need for a fresh ephemeral on retry, just the
// added proof-of-cookie. Run this concurrently with level5_attack_client
// to see legitimate handshakes still complete (at the cost of one extra
// round trip each) while the flood gets absorbed cheaply.
//
// Wire: type(1) | conn_id(8) | noise_msg1(with mac1) | mac2(16, zero on
// the first attempt of each handshake, real once a cookie is known).

#include "../net/anti_dos.hpp"
#include "../net/noise_ik.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr size_t hdr_len = 1 + 8;
constexpr size_t mac2_len = anti_dos::cookie_len;

uint64_t random_token() {
    uint64_t t;
    crypto::random_bytes(&t, sizeof(t));
    return t;
}

crypto::public_key parse_hex_pubkey(std::string_view hex) {
    crypto::public_key pk{};
    if (hex.size() != crypto::key_len * 2) throw std::runtime_error("--server-pubkey must be 64 hex chars");
    for (size_t i = 0; i < crypto::key_len; ++i) {
        pk[i] = (uint8_t)std::stoul(std::string(hex.substr(i * 2, 2)), nullptr, 16);
    }
    return pk;
}

// Sends `wire` and waits (with the socket's configured timeout) for a
// reply matching conn_id. Returns the reply type byte and payload, or
// nullopt on timeout/mismatch.
struct reply_msg {
    uint8_t type;
    std::vector<uint8_t> payload;
};

std::optional<reply_msg> send_and_wait(int fd, const std::vector<uint8_t>& wire, uint64_t conn_id) {
    if (send(fd, wire.data(), wire.size(), 0) < 0) return std::nullopt;
    std::array<uint8_t, 2048> rbuf;
    ssize_t n = recv(fd, rbuf.data(), rbuf.size(), 0);
    if (n <= (ssize_t)hdr_len) return std::nullopt;
    uint64_t reply_conn_id;
    std::memcpy(&reply_conn_id, rbuf.data() + 1, 8);
    if (reply_conn_id != conn_id) return std::nullopt;
    return reply_msg{rbuf[0], std::vector<uint8_t>(rbuf.begin() + hdr_len, rbuf.begin() + n)};
}

} // namespace

int main(int argc, char** argv) {
    std::string_view host = "127.0.0.1";
    uint16_t port = 9992;
    unsigned count = 20;
    std::string_view server_pubkey_hex;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (arg == "--count" && i + 1 < argc) count = (unsigned)std::atoi(argv[++i]);
        else if (arg == "--server-pubkey" && i + 1 < argc) server_pubkey_hex = argv[++i];
    }
    if (server_pubkey_hex.empty()) {
        fprintf(stderr, "usage: %s --server-pubkey <64 hex> [--host H] [--port P] [--count N]\n", argv[0]);
        return 1;
    }
    crypto::public_key server_pub = parse_hex_pubkey(server_pubkey_hex);
    auto client_static = crypto::x25519_generate();

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, std::string(host).c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host\n");
        return 1;
    }
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) < 0) { perror("connect"); return 1; }

    uint64_t succeeded = 0, failed = 0, cookie_retries_used = 0;
    auto overall_start = std::chrono::steady_clock::now();

    for (unsigned i = 0; i < count; ++i) {
        uint64_t conn_id = random_token();
        noise::noise_ik_initiator initiator(client_static, server_pub);
        auto msg1 = initiator.write_message1(nullptr, 0);

        auto build_wire = [&](const anti_dos::cookie& mac2) {
            std::vector<uint8_t> wire(hdr_len + msg1.size() + mac2_len);
            wire[0] = 1;
            std::memcpy(wire.data() + 1, &conn_id, 8);
            std::memcpy(wire.data() + hdr_len, msg1.data(), msg1.size());
            std::memcpy(wire.data() + hdr_len + msg1.size(), mac2.data(), mac2_len);
            return wire;
        };

        anti_dos::cookie zero_mac2{};
        zero_mac2.fill(0);
        bool ok = false;

        auto reply = send_and_wait(fd, build_wire(zero_mac2), conn_id);
        if (reply && reply->type == 4) {
            anti_dos::cookie cookie_value;
            std::memcpy(cookie_value.data(), reply->payload.data(), anti_dos::cookie_len);
            auto mac2 = anti_dos::compute_mac2(cookie_value, msg1.data(), msg1.size());
            ++cookie_retries_used;
            reply = send_and_wait(fd, build_wire(mac2), conn_id);
        }

        if (reply && reply->type == 2) {
            try {
                initiator.read_message2(reply->payload);
                ok = true;
            } catch (const noise::handshake_error&) {
            }
        }

        if (ok) ++succeeded; else ++failed;
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - overall_start).count();
    fprintf(stderr,
            "[client] succeeded=%llu failed=%llu cookie_retries_used=%llu elapsed=%.2fs "
            "handshakes_per_sec=%.1f\n",
            (unsigned long long)succeeded, (unsigned long long)failed,
            (unsigned long long)cookie_retries_used, elapsed,
            (double)succeeded / elapsed);
    return 0;
}
