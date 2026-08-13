#pragma once
// ============================================================================
// NOT PART OF THE SUPPORTED SURFACE as of the first stable release.
//
// The field engine is the one storage this project supports, documents and
// tests. This file is kept because it works and because its measurements are
// what justified that choice - deleting it would delete the evidence - but it
// has no test coverage, no specification, and no compatibility promise. It is
// still compiled by the benches that use it, so it cannot rot silently.
//
// Read it as a record of an experiment, not as an API.
// ============================================================================
// uring_gather.hpp - reading records through io_uring instead of through the
// mapping, so that more than one read can be outstanding at a time.
//
// WHY THIS EXISTS. Every read the engine has done so far has a queue depth of
// exactly one. A page fault is a thread stopping on a single request; there is
// no way to say "fetch these sixty-four pages" through a dereference. Measured
// at 26 GB the random-read rates were 2880, 6453 and 17549 ops/sec across
// engines and runs, with 112194 major faults per 200000 reads - those numbers
// are the latency of one device request at a time, not the device's capacity.
// An NVMe drive reaches its throughput only with many requests in flight.
//
// WHERE THE DEPTH COMES FROM, since io_uring cannot invent it. On the network
// side (net/multishot_udp.hpp) one SQE and 4096 provided buffers produce depth
// for free, because packets ARRIVE - the outside world is the source of
// concurrency. Nothing arrives at a storage engine. A read happens only
// because someone asked, so the depth equals the number of requests the caller
// has outstanding, and a single-threaded `for (id : ids) get(id)` has exactly
// one. There is no multishot read and there cannot be one.
//
// This file supplies depth from the two places that do not need the caller to
// become asynchronous:
//
//   ACROSS RECORDS - read_many() takes a span of ids. An index range scan, a
//   join, a transaction touching many rows: all of them know their whole set
//   up front and currently spend it one blocking fault at a time.
//
//   WITHIN ONE RECORD - and this is the part that only the field layout can
//   offer. The slot holds the base's offset AND every override's offset, in a
//   resident structure, so a record with d overridden fields is 1+d
//   INDEPENDENT reads whose addresses are all known before any of them is
//   issued. Under mmap that was a cost: the field engine took 112194 major
//   faults per 200000 reads against the in-place engine's 86273, precisely
//   because it read from several places instead of one. Issued to a ring those
//   same several places are read in parallel, and the gather costs one
//   request's latency rather than the sum of 1+d.
//
//   The in-place engine has nothing to gain here. Its record is contiguous, so
//   a read is one request and there is no parallelism to extract. The field
//   engine's worst property under synchronous I/O is its best one under
//   asynchronous I/O.
//
// WHAT THIS IS NOT. Buffered reads, not O_DIRECT: the page cache is still
// underneath, so a cached record is served without touching the device and a
// 512 MB dataset on a 25 GB machine will show the CPU cost of the ring and
// nothing else. That measurement is worth having - it bounds the overhead one
// pays for the option - but the benefit needs either a dataset that does not
// fit or O_DIRECT, which additionally requires 512/4096-byte alignment of
// offset, length and buffer, and therefore a block pool this does not have.

#include "field_storage.hpp"
#include "../net/uring_ring.hpp"

#include <span>
#include <vector>

namespace storage {

    class uring_gather {
    public:
        // `depth` is how many reads may be outstanding. It bounds the ring, so
        // a batch larger than this is submitted in waves rather than refused.
        explicit uring_gather(unsigned depth = 256) : ring(depth), capacity(depth) {}

        uring_gather(const uring_gather&) = delete;
        uring_gather& operator=(const uring_gather&) = delete;

        struct result { std::size_t records = 0, reads = 0, waves = 0, failures = 0; };

        // Reads every id into out[i * stride], with every piece of every record
        // in flight up to `depth`. Records whose pieces overlap (only run-time
        // patch() can make that happen) are filled by the ordered path instead,
        // so the answer is the same either way.
        template <typename engine>
        result read_many(const engine& e, std::span<const record_id> ids, std::byte* out, std::size_t stride) {
            result r{};
            const int fd = e.log_fd();
            unsigned pending = 0;

            // A ROLLING PREFETCH, not a separate resolve pass. The read is a
            // dependency chain - slot in RAM, then base and overrides on the
            // device - and the slot half cannot be overlapped with the data
            // half of the SAME record. Across records it can: by the time id[i]
            // is being turned into SQEs, id[i + lookahead]'s slot is already on
            // its way in.
            constexpr std::size_t lookahead = 16;
            for (std::size_t k = 0; k < lookahead && k < ids.size(); ++k) e.prefetch(ids[k]);

            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (i + lookahead < ids.size()) e.prefetch(ids[i + lookahead]);
                const record_id id = ids[i];
                const uint32_t len = e.length_of(id);
                if (!len) continue;
                std::byte* dst = out + i * stride;

                // Counted first: a record whose pieces would not fit the ring
                // in one go still has to be issued atomically, because a
                // partially submitted record is a partially filled buffer.
                unsigned pieces = 0;
                const bool disjoint = e.for_each_piece(id, [&](std::size_t, uint32_t, uint32_t) { ++pieces; });
                ++r.records;
                if (!disjoint || pieces > capacity) {
                    // Overlapping pieces, or more pieces than the ring can hold
                    // atomically - a partially submitted record is a partially
                    // filled buffer, so it goes through the ordered path whole.
                    fill_ordered(e, id, dst, len);
                    continue;
                }
                if (pending + pieces > capacity) { flush(pending, r); pending = 0; }

                e.for_each_piece(id, [&](std::size_t from, uint32_t at, uint32_t plen) {
                    io_uring_sqe* sqe = ring.get_sqe();
                    io_uring_prep_read(sqe, fd, dst + at, plen, (__u64)from);
                    io_uring_sqe_set_data64(sqe, plen);
                    ++pending;
                });
                r.reads += pieces;
            }
            if (pending) flush(pending, r);
            return r;
        }

    private:
        void flush(unsigned pending, result& r) {
            ring.submit_and_wait(pending);
            unsigned seen = 0;
            while (seen < pending) {
                seen += ring.drain_cqes([&](io_uring_cqe* cqe) {
                    if (cqe->res < 0 || (unsigned)cqe->res != io_uring_cqe_get_data64(cqe)) ++r.failures;
                });
                if (seen < pending) ring.submit_and_wait(pending - seen);
            }
            ++r.waves;
        }

        template <typename engine>
        static void fill_ordered(const engine& e, record_id id, std::byte* dst, uint32_t len) {
            e.read_raw(id, std::span<std::byte>{ dst, len });
        }

        uring_ring ring;
        unsigned capacity;
    };

} // namespace storage
