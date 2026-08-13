#pragma once
// io_backend.hpp - what protocol_endpoint needs from the network, and nothing
// more.
//
// The endpoint used to take a uring_ring& and hand CQEs around. Everything else
// in net/ - the handshake, reassembly, AEAD, the replay window, anti-DoS,
// reliability - never mentioned io_uring at all, so the coupling was four
// places in one file standing between a portable protocol and a portable one.
//
// A backend is anything with these three:
//
//   void arm();
//   bool send(uint8_t type, uint64_t conn_id, const uint8_t* body, size_t len,
//             const sockaddr* addr, socklen_t addr_len);
//   void poll(std::chrono::milliseconds timeout, auto&& on_datagram);
//
// There is no base class and no virtual call: the endpoint is templated on the
// backend, the same way replication is templated on nothing and simply takes
// the table as `auto&`. A concept would document it; a vtable would cost a
// dispatch per packet on the one path that cannot afford one.
//
// send() returns false when the transport has no buffer to put the datagram in.
// That is not an error the protocol can fix - it means the kernel has not
// returned buffers as fast as this is filling them - but it is worth counting,
// so the endpoint does.
//
// poll() waits up to `timeout` and then delivers everything that arrived. A
// zero timeout drains what is already there without blocking. This replaced an
// API that handed the caller io_uring completion events: a CQE is an io_uring
// concept and had no business in an application's loop, and every caller was
// writing the same submit-then-drain dance around it.

#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

namespace protocol {

    // One byte of type, then an 8-byte caller-assigned conn_id. Every packet
    // shares it, and a backend writes it because a backend is what owns the
    // buffer being written into.
    constexpr std::size_t hdr_len = 1 + 8;

    // A received packet, valid only for the duration of the callback it is
    // handed to. Backends differ in where these bytes live - a kernel-provided
    // buffer ring under io_uring, a plain array under epoll - and neither
    // promises anything after the callback returns.
    struct datagram {
        const uint8_t* payload;
        std::size_t payload_len;
        const sockaddr* peer_addr;
        socklen_t peer_addr_len;
    };

} // namespace protocol
