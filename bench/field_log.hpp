#pragma once
// field_log.hpp - a prototype of a different shape for the same engine.
//
// The shipping design keeps TWO regions: an append-only log, and a
// materialization of every live record that update() patches in place. A read
// is one pointer dereference; an update is an append plus one random write.
//
// This one keeps ONE file, the log, and nothing else. Nothing is ever patched.
// An update appends the changed FIELD and then only moves a pointer: the index
// records, per record, where the base CREATE is and where the newest version
// of each overridden field is. A read gathers the record from those pieces.
//
// It is Bitcask with the granularity moved from the record to the field.
//
// The trade it makes, stated before any measurement:
//
//   gives up   a read is no longer one access. A record with k overridden
//              fields costs 1 + k lookups, scattered arbitrarily across the
//              log, and get() can no longer hand back a pointer - the record
//              is not contiguous anywhere, so it has to be assembled.
//
//   gains      half the disk, because there is no second copy. No random
//              write on the update path at all, which is what costs the
//              shipping design its major faults once the data exceeds RAM.
//              And no write amplification: changing eight bytes of a kilobyte
//              record writes eight bytes, where a B+tree or a heap table
//              rewrites the whole row.
//
// MEASURED against the shipping design, 26 GB of data on a 31 GB machine
// (bench/big_dataset.cpp, identical workload, 1 KB records):
//
//                          shipping        this      ratio
//   fill (rec/sec)           247213      236050       0.96x
//   random read                2920       10104        3.5x
//   random update              8828    11357195       1287x
//   read hot, before          18556       46661        2.5x
//   hot update x5 (rec/sec)    5536    19914309       3597x
//   read hot, after        33979337     4830630       0.14x
//   major faults, reads       78472       27057
//   major faults, updates     22013           0
//   on disk                  56.2 GB     28.0 GB
//
// The update numbers are the ones the design predicts: an update here touches
// no page of the record at all, so it takes ZERO major faults, while patching
// in place takes one random write into a 26 GB region and 22013 of them.
//
// The read number is the one it does NOT predict, and the reason is worth
// more than the number. Reads got 3.5x FASTER, not slower, because half the
// footprint means twice as much of it fits in the page cache: 27057 faults
// against 78472 for the same 200k reads. The second region was not just disk,
// it was competing for memory with the first one.
//
// What did NOT show up is the cost this design is supposed to pay. "read hot,
// after" is slower than "before" for the gather, but both are in cache here,
// because the overrides were written moments earlier. A workload where a
// record is updated and then read much later, with its overrides long evicted,
// would pay one extra random read per overridden field - and this benchmark
// does not build one. Treat the read column as "no penalty when the overrides
// are warm", not as "no penalty".
//
// collapse() is the safety valve: once a record has accumulated too many
// overrides, it is rewritten as one fresh CREATE and the chain is dropped, so
// the gather depth stays bounded rather than growing with update history.
// That is the same move an LSM makes with compaction and Postgres makes when
// it prunes a HOT chain.

#include "../storage/storage.hpp"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace fieldlog {

    using storage::record_id;
    using storage::record_header;
    using storage::tag_t;
    using storage::record_op;
    using storage::pad8;
    using storage::checksum_of;

    inline constexpr uint32_t no_node = 0xFFFFFFFFu;

    // Where the newest version of one field lives. Chained per record; an
    // update to a field already overridden replaces the node rather than
    // adding one, so the chain is bounded by DISTINCT fields touched, not by
    // the number of updates.
    struct override_node {
        uint64_t log_offset;   // where the new bytes are in the log
        uint32_t field_offset; // where they belong inside the record
        uint32_t len;
        uint32_t next;
        uint32_t pad;
    };

    struct slot {
        uint64_t base;      // log offset of the CREATE's payload, 0 = no record
        uint32_t head;      // first override, or no_node
        uint32_t overrides; // how deep the chain is
    };

    class store {
    public:
        store(const char* path, std::size_t capacity, std::size_t max_ids, std::size_t max_overrides)
            : cap(capacity), slots(max_ids), nodes(max_overrides) {
            fd = ::open(path, O_RDWR | O_CREAT, 0644);
            if (fd < 0) return;
            if (::posix_fallocate(fd, 0, (off_t)capacity) != 0) return;
            base = (std::byte*)::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) base = nullptr;
            for (auto& s : slots) { s.base = 0; s.head = no_node; s.overrides = 0; }
        }
        ~store() {
            if (base) ::munmap(base, cap);
            if (fd >= 0) ::close(fd);
        }
        store(const store&) = delete;
        store& operator=(const store&) = delete;

        bool ok() const noexcept { return base != nullptr; }

        record_id create(tag_t tag, const void* bytes, uint32_t len) {
            const record_id id = next_id++;
            if (id >= slots.size()) return 0;
            slots[id].base = (uint64_t)append(record_op::create, id, 0, tag, bytes, len);
            slots[id].head = no_node;
            slots[id].overrides = 0;
            return id;
        }

        // The whole point: append the field, then move a pointer. No page of
        // the record itself is touched, wherever it happens to live.
        void update(record_id id, uint32_t field_offset, const void* bytes, uint32_t len) {
            if (id >= slots.size() || !slots[id].base) return;
            const std::size_t at = append(record_op::update, id, field_offset, 0, bytes, len);

            for (uint32_t n = slots[id].head; n != no_node; n = nodes[n].next) {
                if (nodes[n].field_offset == field_offset && nodes[n].len == len) {
                    nodes[n].log_offset = at;   // same field again - replace, do not grow
                    return;
                }
            }
            if (used_nodes >= nodes.size()) return;
            const uint32_t n = (uint32_t)used_nodes++;
            nodes[n] = { at, field_offset, len, slots[id].head, 0 };
            slots[id].head = n;
            ++slots[id].overrides;
        }

        void destroy(record_id id) {
            if (id >= slots.size() || !slots[id].base) return;
            append(record_op::destroy, id, 0, 0, nullptr, 0);
            slots[id].base = 0;
            slots[id].head = no_node;
            slots[id].overrides = 0;
        }

        // A read is a gather, not a dereference: the base record, then every
        // field that has been overridden since.
        bool get(record_id id, void* out, uint32_t len) const {
            if (id >= slots.size() || !slots[id].base) return false;
            std::memcpy(out, base + slots[id].base, len);
            for (uint32_t n = slots[id].head; n != no_node; n = nodes[n].next) {
                const override_node& o = nodes[n];
                if ((std::size_t)o.field_offset + o.len <= len)
                    std::memcpy((std::byte*)out + o.field_offset, base + o.log_offset, o.len);
            }
            return true;
        }

        // Rewrites the record as one fresh CREATE and drops its chain, so the
        // next read is a single access again. The old bytes stay in the log as
        // garbage until a compaction reclaims them.
        void collapse(record_id id, void* scratch, uint32_t len, tag_t tag) {
            if (id >= slots.size() || !slots[id].base || slots[id].head == no_node) return;
            get(id, scratch, len);
            slots[id].base = (uint64_t)append(record_op::create, id, 0, tag, scratch, len);
            slots[id].head = no_node;
            slots[id].overrides = 0;
        }

        uint32_t depth(record_id id) const noexcept { return id < slots.size() ? slots[id].overrides : 0; }
        std::size_t current_offset() const noexcept { return write_offset; }
        std::size_t overrides_used() const noexcept { return used_nodes; }
        std::size_t index_bytes() const noexcept {
            return slots.size() * sizeof(slot) + nodes.size() * sizeof(override_node);
        }

    private:
        // Same record format as the shipping engine, checksum included, so
        // this is a different index over the same log rather than a different
        // file format.
        std::size_t append(record_op op, record_id id, uint32_t offset, tag_t tag, const void* bytes, uint32_t len) {
            record_header hdr{ id, offset, len, op, 0, tag };
            hdr.checksum = checksum_of(hdr, bytes, len);
            std::byte* at = base + write_offset;
            std::memcpy(at, &hdr, sizeof(hdr));
            if (len) std::memcpy(at + sizeof(hdr), bytes, len);
            const std::size_t payload = write_offset + sizeof(hdr);
            write_offset += pad8(sizeof(hdr) + len);
            return payload;
        }

        std::byte* base = nullptr;
        std::size_t cap = 0;
        std::size_t write_offset = 0;
        record_id next_id = 1;
        int fd = -1;
        std::vector<slot> slots;
        std::vector<override_node> nodes;
        std::size_t used_nodes = 0;
    };

} // namespace fieldlog
