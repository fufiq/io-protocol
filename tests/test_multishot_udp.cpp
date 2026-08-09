#include <doctest/doctest.h>

#include "../net/multishot_udp.hpp"
#include "../net/uring_ring.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace {

constexpr uint64_t recv_tag = 1;

int make_bound_udp_socket(uint16_t* out_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // let the kernel pick an ephemeral port
    REQUIRE(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);

    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(fd, (sockaddr*)&addr, &len) == 0);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

} // namespace

TEST_CASE("multishot receiver delivers a single datagram with correct payload and peer address") {
    uint16_t server_port = 0;
    int server_fd = make_bound_udp_socket(&server_port);
    uint16_t client_port = 0;
    int client_fd = make_bound_udp_socket(&client_port);

    uring_ring ring(64);
    multishot_udp_receiver recv(ring, server_fd, recv_tag, /*num_bufs=*/16, /*buf_size=*/256);
    recv.arm();
    ring.submit();

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(server_port);
    const char msg[] = "hello multishot";
    REQUIRE(sendto(client_fd, msg, sizeof(msg), 0, (sockaddr*)&dst, sizeof(dst)) ==
            (ssize_t)sizeof(msg));

    ring.submit_and_wait(1);
    bool delivered = false;
    unsigned n = ring.drain_cqes([&](io_uring_cqe* cqe) {
        if (io_uring_cqe_get_data64(cqe) != recv_tag) return;
        auto frag = recv.handle_cqe(cqe);
        REQUIRE(frag.has_value());
        CHECK(frag->payload_len == sizeof(msg));
        CHECK(std::memcmp(frag->payload, msg, sizeof(msg)) == 0);

        REQUIRE(frag->peer_addr_len >= sizeof(sockaddr_in));
        auto* peer = (const sockaddr_in*)frag->peer_addr;
        CHECK(peer->sin_family == AF_INET);
        CHECK(ntohs(peer->sin_port) == client_port);
        delivered = true;
    });
    recv.flush();

    CHECK(n == 1);
    CHECK(delivered);
    CHECK(recv.malformed_count() == 0);

    close(client_fd);
    close(server_fd);
}

TEST_CASE("multishot receiver handles many datagrams without needing to re-arm") {
    uint16_t server_port = 0;
    int server_fd = make_bound_udp_socket(&server_port);
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(client_fd >= 0);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(server_port);

    uring_ring ring(256);
    multishot_udp_receiver recv(ring, server_fd, recv_tag, /*num_bufs=*/64, /*buf_size=*/256);
    recv.arm();
    ring.submit();

    constexpr int count = 30;
    for (int i = 0; i < count; ++i) {
        char msg[16];
        std::snprintf(msg, sizeof(msg), "%03d", i);
        REQUIRE(sendto(client_fd, msg, sizeof(msg), 0, (sockaddr*)&dst, sizeof(dst)) >= 0);
    }

    int received = 0;
    while (received < count) {
        ring.submit_and_wait(1);
        ring.drain_cqes([&](io_uring_cqe* cqe) {
            if (io_uring_cqe_get_data64(cqe) != recv_tag) return;
            auto frag = recv.handle_cqe(cqe);
            if (frag.has_value()) ++received;
        });
        recv.flush();
    }

    CHECK(received == count);
    CHECK(recv.malformed_count() == 0);

    close(client_fd);
    close(server_fd);
}

TEST_CASE("multishot_udp_receiver rejects a non-power-of-two num_bufs") {
    uint16_t port = 0;
    int fd = make_bound_udp_socket(&port);
    uring_ring ring(64);
    CHECK_THROWS_AS(multishot_udp_receiver(ring, fd, recv_tag, /*num_bufs=*/100, 256), std::invalid_argument);
    close(fd);
}
