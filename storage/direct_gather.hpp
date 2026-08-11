#pragma once
// direct_gather.hpp - the same ring, but reading with O_DIRECT, so the page
// cache is out of the path entirely and every read is a device read.
//
// WHY THIS IS A SEPARATE FILE FROM uring_gather.hpp. Buffered io_uring reads
// measured 6.8x SLOWER than mmap on cached data (722265 against 4938251 ops/sec
// at 512 MB, zero major faults) and depth bought 9% between 8 and 512, because
// there was no device latency to overlap. That is the honest result for a ring
// over the page cache: it is a worse memcpy. O_DIRECT changes the question -
// there is no cached case any more, every read costs a device round trip, and
// depth is the only thing that can pay for it.
//
// WHAT O_DIRECT DEMANDS, and it is why this is a block pool rather than a flag:
// the file offset, the length AND the buffer address must all be multiples of
// the logical block size. Records are 1024 bytes at arbitrary 8-aligned
// offsets, so nothing about a record satisfies that. Every piece therefore
// becomes a read of the 4096-byte blocks that COVER it, into an aligned pool
// slot, followed by a copy of the piece out of that slot.
//
// The cost of that is read amplification, and it is not small: a 1024-byte
// piece covered by one block is 4x, and a piece straddling a boundary is 8x.
// It is stated here rather than discovered later - a design that reads 4 KB to
// deliver 1 KB has given up three quarters of the device's bandwidth before
// any parallelism is applied.
//
// COHERENCY, which is a correctness matter and not a tuning one. The engine
// WRITES through a shared mapping, which is buffered. Mixing buffered writes
// with O_DIRECT reads on the same file gives no guarantee that the read sees
// the write. So this can only read a region that has been msync'd - the caller
// must sync() before gathering, and a record appended since the last sync is
// not readable this way. That is a real restriction on where this path can be
// used: recovery, compaction and cold reads, not the tail of a live log.

#include "field_storage.hpp"
#include "../net/uring_ring.hpp"

#include <cstdlib>
#include <span>
#include <vector>

namespace storage {

    class direct_gather {
    public:
        // block_size must match the device's logical block size or be a
        // multiple of it. 4096 is safe on NVMe, including 512e drives.
        direct_gather(const char* path, unsigned depth = 256,
                      std::size_t max_piece_bytes = 4096, std::size_t block_size = 4096)
            : ring(depth), capacity(depth), block(block_size),
              slot_bytes(((max_piece_bytes + block_size - 1) & ~(block_size - 1)) + block_size) {
            fd = ::open(path, O_RDONLY | O_DIRECT);
            if (fd < 0) return;
            if (::posix_memalign((void**)&pool, block_size, (std::size_t)depth * slot_bytes) != 0) pool = nullptr;
            queued.reserve((std::size_t)depth * 2);
        }

        ~direct_gather() {
            if (pool) ::free(pool);
            if (fd >= 0) ::close(fd);
        }

        direct_gather(const direct_gather&) = delete;
        direct_gather& operator=(const direct_gather&) = delete;

        bool ok() const noexcept { return fd >= 0 && pool; }

        struct result { std::size_t records = 0, reads = 0, waves = 0, failures = 0, block_bytes = 0; };

        // Same contract as uring_gather::read_many, but the OVERLAPPING
        // decomposition underneath: whole base, then overrides on top. Safe
        // here only because the bytes land in this reader's own pool and the
        // copies out of it are applied in submission order after the wave.
        template <typename engine>
        result read_many(const engine& e, std::span<const record_id> ids, std::byte* out, std::size_t stride) {
            result r{};
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

                unsigned pieces = 0;
                e.for_each_piece_ordered(id, [&](std::size_t, uint32_t, uint32_t) { ++pieces; });
                ++r.records;
                if (pieces > capacity) { e.read_raw(id, std::span<std::byte>{ dst, len }); continue; }
                if (issued + pieces > capacity) flush(r);

                e.for_each_piece_ordered(id, [&](std::size_t from, uint32_t at, uint32_t plen) {
                    const std::size_t first = from & ~(block - 1);
                    const std::size_t last = (from + plen + block - 1) & ~(block - 1);
                    const std::size_t span = last - first;

                    // A piece already covered by a block this wave is fetching
                    // rides along instead of asking for it again. Overrides
                    // written close together in the log - the common case, they
                    // were appended in the same burst - share a block, and an
                    // 8-byte field is not worth a second 4 KB read.
                    for (std::size_t k = wave_start; k < queued.size(); ++k) {
                        if (queued[k].block_at != first || queued[k].span < span) continue;
                        queued.push_back({ dst + at, queued[k].buf + (from - first), plen, 0, first, nullptr });
                        return;
                    }
                    const std::size_t slot = issued++;
                    std::byte* buf = pool + slot * slot_bytes;
                    io_uring_sqe* sqe = ring.get_sqe();
                    io_uring_prep_read(sqe, fd, buf, (unsigned)span, (__u64)first);
                    io_uring_sqe_set_data64(sqe, queued.size());
                    queued.push_back({ dst + at, buf + (from - first), plen, (unsigned)span, first, buf });
                    r.block_bytes += span;
                    ++r.reads;
                });
            }
            if (!queued.empty()) flush(r);
            return r;
        }

    private:
        // The copies are applied AFTER the whole wave, in submission order -
        // which is base first, then its overrides. That ordering is what makes
        // the contiguous-base form usable: the pieces overlap, and completions
        // arrive in whatever order the device chose.
        void flush(result& r) {
            const unsigned pending = (unsigned)issued;
            if (pending) {
                ring.submit_and_wait(pending);
                unsigned seen = 0;
                while (seen < pending) {
                    seen += ring.drain_cqes([&](io_uring_cqe* cqe) {
                        const std::size_t at = (std::size_t)io_uring_cqe_get_data64(cqe);
                        if (cqe->res < 0 || (unsigned)cqe->res < queued[at].span) ++r.failures;
                    });
                    if (seen < pending) ring.submit_and_wait(pending - seen);
                }
            }
            for (const auto& q : queued) std::memcpy(q.dst, q.src, q.len);
            queued.clear();
            issued = 0;
            wave_start = 0;
            ++r.waves;
        }

        struct pending_read {
            std::byte* dst; std::byte* src; unsigned len; unsigned span;
            std::size_t block_at; std::byte* buf;
        };

        uring_ring ring;
        unsigned capacity;
        std::size_t block, slot_bytes;
        int fd = -1;
        std::byte* pool = nullptr;
        std::vector<pending_read> queued;
        std::size_t issued = 0, wave_start = 0;
    };

} // namespace storage
