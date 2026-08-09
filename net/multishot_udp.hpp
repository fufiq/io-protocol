#pragma once

// Shared multishot-recvmsg + provided-buffer-ring UDP receiver. One SQE
// stays posted indefinitely and the kernel keeps handing back CQEs (and
// refilling itself from the buffer pool) instead of needing a fresh SQE
// per packet - benchmarked at ~15-20% higher pps than the classic
// "one recvmsg SQE per slot, re-armed every completion" model (see
// level1_multishot_experiment.cpp, superseded by this class once
// level1_echo_server adopted it).
//
// Only the receive side benefits: there is no multishot sendmsg, so any
// server that replies still issues one sendmsg SQE per outgoing packet -
// this class only ever touches the recv path.
//
// This is deliberately its own class rather than folded into uring_ring:
// uring_ring stays a thin, opcode-agnostic wrapper; this owns buffer-ring
// lifecycle and the io_uring_recvmsg_out parsing, which is fiddly enough
// (and was the site of a real bug class in earlier levels - mixing up
// which CQE belongs to which operation) that duplicating it across every
// server's .cpp file was the actual risk worth designing away.

#include "uring_ring.hpp"

#include <liburing.h>
#include <sys/socket.h>

#include <bit>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

class multishot_udp_receiver {
public:
    // `fd` must already be bound (and, if the caller wants replies, left
    // unconnected so sendmsg can target arbitrary peers). num_bufs must be
    // a power of two (io_uring_setup_buf_ring's own requirement). receive_tag
    // is the io_uring user_data value this receiver's SQEs carry - callers
    // multiplexing other operations (e.g. sendmsg replies) on the same ring
    // must pick a disjoint tag for those, exactly like the send/recv tag
    // split already used in the level2 client benchmarks.
    multishot_udp_receiver(uring_ring& ring, int fd, uint64_t receive_tag, unsigned num_bufs = 4096, unsigned buf_size = 2048, int bgid = 0)
        : ring(ring), fd(fd), receive_tag(receive_tag), num_bufs(num_bufs), buf_size(buf_size), bgid(bgid),
          mask(io_uring_buf_ring_mask(num_bufs)), backing((size_t)num_bufs * buf_size) {
        // num_bufs is a genuinely runtime-configurable knob (CLI-tunable per deployment, unlike
        // reassembler/timer_wheel's window sizes - see their own template-parameter comments for
        // why those became compile-time), so this stays a runtime check. std::has_single_bit
        // correctly rejects 0 too (unlike a hand-rolled `(n & (n-1)) == 0`, which is wrongly true
        // at n=0), so there's no separate zero-check needed alongside it.
        if (!std::has_single_bit(num_bufs)) throw std::invalid_argument("multishot_udp_receiver: num_bufs must be a power of two");

        int err = 0;
        br = io_uring_setup_buf_ring(ring.raw(), num_bufs, bgid, 0, &err);
        if (br == nullptr) throw std::system_error(-err, std::system_category(), "io_uring_setup_buf_ring");
        for (unsigned i = 0; i < num_bufs; ++i) io_uring_buf_ring_add(br, backing.data() + (size_t)i * buf_size, buf_size, (unsigned short)i, mask, (int)i);
        io_uring_buf_ring_advance(br, (int)num_bufs);

        msg_template.msg_name = &name_scratch; // never written into directly; sizes the reserved name area
        msg_template.msg_namelen = sizeof(name_scratch);
    }

    ~multishot_udp_receiver() {
        if (br != nullptr) io_uring_free_buf_ring(ring.raw(), br, num_bufs, bgid);
    }

    multishot_udp_receiver(const multishot_udp_receiver&) = delete;
    multishot_udp_receiver& operator=(const multishot_udp_receiver&) = delete;

    // (Re-)posts the multishot recvmsg SQE. Must be called once at startup
    // and again any time a completion comes back without
    // IORING_CQE_F_MORE set (the previous multishot request has ended,
    // e.g. after an error).
    void arm() {
        io_uring_sqe* sqe = ring.get_sqe();
        io_uring_prep_recvmsg_multishot(sqe, fd, &msg_template, 0);
        io_uring_sqe_set_buf_group(sqe, bgid);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        io_uring_sqe_set_data64(sqe, receive_tag);
        needs_rearm = false;
    }

    struct fragment {
        const uint8_t* payload;
        size_t payload_len;
        const sockaddr* peer_addr;
        socklen_t peer_addr_len;
    };

    // Call for every CQE whose user_data == tag(). Returns nullopt for a
    // malformed/error completion (already counted internally via
    // malformed_count()); the underlying buffer is automatically queued for
    // return to the kernel pool either way - callers must not hold
    // `fragment::payload` past the next flush() call.
    std::optional<fragment> handle_cqe(const io_uring_cqe* cqe) {
        if (cqe->res < 0 || !(cqe->flags & IORING_CQE_F_BUFFER)) {
            if (!(cqe->flags & IORING_CQE_F_MORE)) needs_rearm = true;
            if (cqe->res < 0) ++malformed;
            return std::nullopt;
        }

        unsigned buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        uint8_t* buf_ptr = backing.data() + (size_t)buf_id * buf_size;

        if (!(cqe->flags & IORING_CQE_F_MORE)) needs_rearm = true;

        auto* out = io_uring_recvmsg_validate(buf_ptr, cqe->res, &msg_template);
        io_uring_buf_ring_add(br, buf_ptr, buf_size, (unsigned short)buf_id, mask, pending_return_offset++);
        if (out == nullptr) {
            ++malformed;
            return std::nullopt;
        }

        fragment frag;
        frag.payload = (uint8_t*)io_uring_recvmsg_payload(out, &msg_template);
        frag.payload_len = io_uring_recvmsg_payload_length(out, cqe->res, &msg_template);
        frag.peer_addr = (sockaddr*)io_uring_recvmsg_name(out);
        frag.peer_addr_len = out->namelen;
        return frag;
    }

    // Applies every buffer release queued up by handle_cqe() calls since
    // the last flush(), and re-arms if any handled CQE signaled the
    // multishot request ended. Call once per event-loop iteration after
    // draining the whole CQE batch - batched for the same reason SQ
    // submission and CQ draining are batched everywhere else in this
    // codebase.
    void flush() {
        if (pending_return_offset > 0) {
            io_uring_buf_ring_advance(br, pending_return_offset);
            pending_return_offset = 0;
        }
        if (needs_rearm) arm();
    }

    uint64_t tag() const { return receive_tag; }
    uint64_t malformed_count() const { return malformed; }

private:
    uring_ring& ring;
    int fd;
    uint64_t receive_tag;
    unsigned num_bufs;
    unsigned buf_size;
    int bgid;
    int mask;

    std::vector<uint8_t> backing;
    io_uring_buf_ring* br = nullptr;
    msghdr msg_template{};
    sockaddr_storage name_scratch{}; // only used for its size; multishot writes the real peer addr into `backing`

    int pending_return_offset = 0;
    bool needs_rearm = false;
    uint64_t malformed = 0;
};
