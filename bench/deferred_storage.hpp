#pragma once
// deferred_storage.hpp - a prototype of the third option: merge LATER.
//
// storage.hpp and field_storage.hpp differ in WHEN the new value gets moved
// from the log tail to where the record lives. One does it immediately, and
// pays a random write on every update. The other never does it, and pays a
// gather on every read. Both answers are forced by the same fact: a read that
// returns a pointer needs the record contiguous, and an update that writes
// only the log tail leaves it somewhere else.
//
// This does it in between:
//
//   update    append the diff to the log, hang it on a per-record pending
//             chain, and set a dirty bit. NO random write.
//   read      if the record is clean, return a pointer, exactly as
//             storage.hpp does. If it is dirty, settle it first - apply its
//             chain and clear the bit - and then return a pointer. The API
//             does not change.
//   flush     apply every remaining chain SORTED BY ADDRESS, turning what
//             would have been N random writes into one ordered sweep.
//
// Two things this buys that neither of the others can:
//
//   * the writes are sorted. The same pages get dirtied, but in address
//     order, which a device handles far better than the same count scattered.
//   * repeated updates to one record COALESCE. A chain entry for a field that
//     is updated again is replaced, so N updates to one record cost one write
//     at flush instead of N.
//
// The dirty bit is free: index offsets are pad8-aligned, so bit 0 of the
// offset is always zero and can carry the flag without widening the entry.
//
// It keeps storage.hpp's two regions, so it does NOT fix the 2x disk
// footprint. This prototype is about the read/update axis only.

#include "../storage/storage.hpp"

#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace deferred {

    using storage::record_id;
    using storage::record_header;
    using storage::tag_t;
    using storage::record_op;
    using storage::pad8;
    using storage::checksum_of;
    using storage::tagged_aggregate;
    using storage::member_class_t;
    using storage::member_value_t;

    inline constexpr uint32_t no_node = 0xFFFFFFFFu;

    struct pending_node {
        uint64_t log_offset;
        uint32_t field_offset;
        uint32_t len;
        uint32_t next;
        uint32_t pad;
    };

    class store {
    public:
        store(const char* log_path, const char* data_path, std::size_t capacity, std::size_t max_ids)
            : cap(capacity), index(max_ids, 0) {
            log_fd = ::open(log_path, O_RDWR | O_CREAT, 0644);
            data_fd = ::open(data_path, O_RDWR | O_CREAT, 0644);
            if (log_fd < 0 || data_fd < 0) return;
            if (::posix_fallocate(log_fd, 0, capacity) != 0) return;
            if (::posix_fallocate(data_fd, 0, capacity) != 0) return;
            base = (std::byte*)::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, log_fd, 0);
            data = (std::byte*)::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
            if (base == MAP_FAILED || data == MAP_FAILED) { base = nullptr; return; }
            nodes.reserve(1 << 20);
        }
        ~store() {
            if (base && base != MAP_FAILED) ::munmap(base, cap);
            if (data && data != MAP_FAILED) ::munmap(data, cap);
            if (log_fd >= 0) ::close(log_fd);
            if (data_fd >= 0) ::close(data_fd);
        }
        store(const store&) = delete;
        store& operator=(const store&) = delete;

        bool ok() const noexcept { return base && data; }

        template <tagged_aggregate T>
        record_id create(const T& value) {
            const record_id id = next_id++;
            if (id >= index.size()) return 0;
            auto bytes = std::as_bytes(std::span{&value, 1});
            append(record_op::create, id, 0, T::tag, bytes);
            std::size_t at = data_offset;
            write_at(data, data_offset, record_op::create, id, 0, T::tag, bytes);
            index[id] = at + sizeof(record_header); // 8-aligned, so bit 0 stays free
            return id;
        }

        // The whole point: no write to the data region here.
        template <auto member>
        void update(record_id id, member_value_t<member> value) {
            if (id >= index.size() || !index[id]) return;
            const uint32_t off = (uint32_t)(std::size_t)&(((member_class_t<member> const volatile*)0)->*member);
            auto bytes = std::as_bytes(std::span{&value, 1});
            const std::size_t at = append(record_op::update, id, off, 0, bytes);

            uint32_t& head = heads[id];
            if (!(index[id] & dirty_bit)) { head = no_node; index[id] |= dirty_bit; }
            for (uint32_t n = head; n != no_node; n = nodes[n].next)
                if (nodes[n].field_offset == off && nodes[n].len == bytes.size()) {
                    nodes[n].log_offset = at;   // coalesce: the same field again is one write, not two
                    return;
                }
            nodes.push_back({ at, off, (uint32_t)bytes.size(), head, 0 });
            head = (uint32_t)(nodes.size() - 1);
        }

        // Still a pointer. A dirty record is settled on the way past, which is
        // a random write - but only for records that are actually read, and at
        // most once however many times they were updated.
        template <tagged_aggregate T>
        const T* get(record_id id) {
            if (id >= index.size() || !index[id]) return nullptr;
            if (index[id] & dirty_bit) settle(id);
            return (const T*)(data + index[id]);
        }

        // Everything still pending, applied in ADDRESS order.
        void flush() {
            if (heads.empty()) return;
            order.clear();
            order.reserve(heads.size());
            for (auto& [id, head] : heads) order.push_back({ index[id] & ~dirty_bit, id });
            std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [addr, id] : order) apply_chain(id, addr);
            heads.clear();
            nodes.clear();
        }

        void sync() {
            flush();
            if (write_offset <= last_synced) return;
            const std::size_t start = last_synced & ~std::size_t(4095);
            ::msync(base + start, write_offset - start, MS_SYNC);
            last_synced = write_offset;
        }

        std::size_t current_offset() const noexcept { return write_offset; }
        std::size_t pending() const noexcept { return heads.size(); }
        std::size_t settles() const noexcept { return settle_count; }

    private:
        static constexpr std::size_t dirty_bit = 1;

        void settle(record_id id) {
            apply_chain(id, index[id] & ~dirty_bit);
            heads.erase(id);
            ++settle_count;
        }

        void apply_chain(record_id id, std::size_t addr) {
            auto it = heads.find(id);
            if (it == heads.end()) { index[id] = addr; return; }
            for (uint32_t n = it->second; n != no_node; n = nodes[n].next)
                std::memcpy(data + addr + nodes[n].field_offset, base + nodes[n].log_offset, nodes[n].len);
            index[id] = addr;
        }

        std::size_t append(record_op op, record_id id, uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            return write_at(base, write_offset, op, id, offset, tag, bytes);
        }

        static std::size_t write_at(std::byte* region, std::size_t& cursor, record_op op, record_id id,
                                    uint32_t offset, tag_t tag, std::span<const std::byte> bytes) {
            record_header hdr{ id, offset, (uint32_t)bytes.size(), op, 0, tag };
            hdr.checksum = checksum_of(hdr, bytes.data(), (uint32_t)bytes.size());
            std::byte* at = region + cursor;
            std::memcpy(at, &hdr, sizeof(hdr));
            if (!bytes.empty()) std::memcpy(at + sizeof(hdr), bytes.data(), bytes.size());
            const std::size_t payload = cursor + sizeof(hdr);
            cursor += pad8(sizeof(hdr) + bytes.size());
            return payload;
        }

        std::byte* base = nullptr;
        std::byte* data = nullptr;
        std::size_t cap = 0, write_offset = 0, data_offset = 0, last_synced = 0, settle_count = 0;
        record_id next_id = 1;
        int log_fd = -1, data_fd = -1;
        std::vector<std::size_t> index;                       // bit 0 = dirty
        std::vector<pending_node> nodes;
        std::unordered_map<record_id, uint32_t> heads;
        std::vector<std::pair<std::size_t, record_id>> order; // scratch for the sorted sweep
    };

} // namespace deferred
