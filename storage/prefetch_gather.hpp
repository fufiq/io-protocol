#pragma once
// prefetch_gather.hpp - queue depth without threads, without pread, without
// io_uring. Portable POSIX, and the read path itself does not change.
//
// THE PROBLEM, in the numbers it was measured with. A single-threaded reader
// over a mapping that does not fit in memory ran at 5731 records/sec while the
// same device, probed directly, sustained 239000 IOPS at queue depth 512. That
// is three multipliers, not one:
//
//   0.42 faults per read     8459 major faults over 20000 reads - not every
//                            record is cold, so the read rate and the fault
//                            rate are different numbers
//   queue depth 1            a page fault is one thread stopped on one
//                            request; the device needs hundreds in flight to
//                            reach 239000, and gives ~12000 with one
//   275 us per fault         against ~100 us of device: the trap, the page
//                            allocation, and - under memory pressure - having
//                            to evict something before there is a page to
//                            read into
//
// This file attacks the middle one only, and says so up front. The fault path
// and the eviction are the price of going through the page cache at all; only
// O_DIRECT avoids them, and O_DIRECT is not portable and not free (measured at
// 8.2x read amplification, since a 4 KB block is the minimum unit for a piece
// of any size).
//
// HOW. Before assembling a batch, hand the kernel the pages that batch is
// about to touch. posix_madvise(POSIX_MADV_WILLNEED) starts the reads and
// returns; several are then in flight while the caller is still assembling the
// records before them. Nothing else changes - reads still go through the same
// assemble(), the same mapping, the same index.
//
// WHY THE FIELD LAYOUT GETS MORE OUT OF THIS than an in-place one. The slot
// holds the base's offset AND every override's offset, in a resident structure,
// so a record with d overridden fields yields 1+d addresses that are all known
// before any of them is touched. A contiguous record yields one. The layout
// whose reads look scattered is the layout that can say in advance where it is
// going.
//
// A NOTE ON madvise, because this project has been burnt by it. MADV_RANDOM on
// this same mapping made everything worse - reads 3262 -> 2516, updates
// 10148 -> 6180, major faults on reads 72k -> 144k. That is a different
// mechanism: a hint about the ACCESS PATTERN, which disables the readahead that
// sequential fill and recovery depend on. WILLNEED names specific pages and
// asks for them once. Related only by the name of the syscall.

#include "field_storage.hpp"

#include <algorithm>
#include <span>
#include <sys/mman.h>
#include <vector>

namespace storage {

    class prefetch_gather {
    public:
        // `window` records are prefetched ahead of the ones being assembled.
        // Too small and the device drains before the next batch is issued; too
        // large and the first pages are evicted again before they are read.
        explicit prefetch_gather(std::size_t window = 64, std::size_t page = 4096)
            : lookahead(window ? window : 1), page_size(page) {
            ranges.reserve(lookahead * 8);
        }

        struct result {
            std::size_t records = 0, pieces = 0, advises = 0, advised_bytes = 0;
        };

        template <typename engine>
        result read_many(const engine& e, std::span<const record_id> ids, std::byte* out, std::size_t stride) {
            result r{};
            const std::byte* base = e.raw_base();
            if (!base) return r;

            // Prime the first window, then keep one window ahead: the device
            // works on batch k+1 while the caller assembles batch k, instead of
            // alternating between a burst of I/O and a burst of memcpy.
            advise_window(e, base, ids, 0, r);

            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (i % lookahead == 0) advise_window(e, base, ids, i + lookahead, r);
                const uint32_t len = e.length_of(ids[i]);
                if (!len) continue;
                (void)e.read_raw(ids[i], std::span<std::byte>{ out + i * stride, len });
                ++r.records;
            }
            return r;
        }

    private:
        // One advise per CONTIGUOUS run rather than per piece. Each call is a
        // syscall, and a record's base plus its overrides frequently land in
        // the same page or in neighbouring ones - overrides written in the same
        // burst are adjacent in the log by construction. Merging turns a few
        // hundred calls into a few dozen and lets the kernel issue larger reads.
        template <typename engine>
        void advise_window(const engine& e, const std::byte* base,
                           std::span<const record_id> ids, std::size_t from, result& r) {
            if (from >= ids.size()) return;
            const std::size_t to = std::min(from + lookahead, ids.size());

            ranges.clear();
            for (std::size_t i = from; i < to; ++i) {
                e.for_each_piece_ordered(ids[i], [&](std::size_t off, uint32_t, uint32_t len) {
                    const std::size_t first = off & ~(page_size - 1);
                    const std::size_t last = (off + len + page_size - 1) & ~(page_size - 1);
                    ranges.push_back({ first, last });
                    ++r.pieces;
                });
            }
            if (ranges.empty()) return;

            std::sort(ranges.begin(), ranges.end(),
                      [](const range& a, const range& b) { return a.first < b.first; });

            std::size_t open_from = ranges[0].first, open_to = ranges[0].last;
            for (std::size_t i = 1; i < ranges.size(); ++i) {
                if (ranges[i].first <= open_to) {         // touching or overlapping
                    open_to = std::max(open_to, ranges[i].last);
                    continue;
                }
                issue(base, open_from, open_to, r);
                open_from = ranges[i].first;
                open_to = ranges[i].last;
            }
            issue(base, open_from, open_to, r);
        }

        void issue(const std::byte* base, std::size_t from, std::size_t to, result& r) {
            // posix_madvise rather than madvise: same effect, and the spelling
            // that exists outside Linux.
            ::posix_madvise((void*)(base + from), to - from, POSIX_MADV_WILLNEED);
            ++r.advises;
            r.advised_bytes += to - from;
        }

        struct range { std::size_t first, last; };

        std::size_t lookahead, page_size;
        std::vector<range> ranges;
    };

} // namespace storage
