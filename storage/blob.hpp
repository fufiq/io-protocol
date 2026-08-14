#pragma once
// blob.hpp - text and binary of a length you do not know in advance.
//
// A record here is a POD aggregate with no padding, so a string inside one can
// only be `char name[40]`: too small for some rows, wasted on most, and a hard
// ceiling either way. That is a real limitation of the TYPED api, and it is
// easy to mistake for a limitation of the engine.
//
// It is not. The engine stores a length per record - field_slot::len, 16 bits -
// and create_raw() takes arbitrary bytes, so records of different sizes already
// coexist under one tag. Verified before this file was written: 0, 1, 5, 300
// and 4000 bytes round-trip, and patch() works on them by offset.
//
// So what was missing was never a capability. It was a convention and a type:
// the text lives as its OWN record, and the record that owns it holds a
// reference instead of an array.
//
//     struct article {
//         static constexpr storage::tag_t tag = 1;
//         storage::blob_ref<2> title;    // tag 2: where the text lives
//         storage::blob_ref<2> body;
//         uint64_t published_ms;
//     };
//
//     article a{};
//     a.title = storage::put_text<2>(table, "A very long title, no ceiling");
//     const auto id = table.create(a);
//     ...
//     std::string title = storage::load_text(table, table.get<article>(id)->title);
//
// WHAT THIS COSTS, stated rather than buried:
//
//   A second record per string. Each carries a 24-byte header, so short strings
//   are worse off than a char[40] would have been - under about 30 bytes, put
//   it in the struct. The win starts where the ceiling used to bite.
//
//   A second lookup to read one. The reference resolves through the slot array,
//   which is one indexed load, and then the bytes are assembled like any
//   record. Reading a row and its text is two gathers, not one.
//
//   No mutation in place. Text is replaced by writing a new blob and pointing
//   the owner at it, which leaves the old one for compaction to reclaim. That
//   is the same trade the whole engine makes and it is not a special case here.
//
// WHAT IS DELIBERATELY ABSENT. Interning - sharing one record between equal
// strings - is not here, because it needs a hash function chosen for the data
// and a decision about collisions, and both are the application's. It is four
// lines over a unique_index on a hash field you already control.

#include "field_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace storage {

    // A reference to a variable-length record stored under `tag_v`.
    //
    // Separate from ref<T> because ref<T> names a TYPE and a blob has none -
    // its payload is the bytes, so there is nothing to get<>() into. The tag is
    // what distinguishes one kind of blob from another, and putting it in the
    // type means a title reference cannot be handed to something expecting a
    // body reference.
    //
    // Eight bytes, trivially copyable, no padding: it satisfies the same
    // aggregate rules a record field must, which is the point of it being a
    // class rather than a typedef.
    template <tag_t tag_v>
    class blob_ref {
    public:
        static constexpr tag_t tag = tag_v;

        constexpr blob_ref() noexcept = default;
        constexpr explicit blob_ref(record_id value) noexcept : id_(value) {}

        constexpr record_id id() const noexcept { return id_; }
        constexpr explicit operator bool() const noexcept { return id_ != 0; }

        friend constexpr bool operator==(blob_ref, blob_ref) noexcept = default;
        friend constexpr auto operator<=>(blob_ref, blob_ref) noexcept = default;

    private:
        record_id id_ = 0;
    };

    // Writes `bytes` as a record of its own and returns a reference to it.
    // A default-constructed reference comes back when the payload is longer
    // than the spec allows, rather than a truncated record that would read as
    // valid - max_record_bytes is a compile-time bound on this engine and
    // silently honouring half of a string is worse than refusing it.
    template <tag_t tag_v>
    [[nodiscard]] blob_ref<tag_v> put_blob(auto& table, std::span<const std::byte> bytes) {
        using spec = typename std::remove_reference_t<decltype(table)>::spec_type;
        if (bytes.size() > spec::max_record_bytes) return blob_ref<tag_v>{};
        const record_id id = table.reserve_id();
        table.create_raw(id, tag_v, bytes);
        return blob_ref<tag_v>(id);
    }

    template <tag_t tag_v>
    [[nodiscard]] blob_ref<tag_v> put_text(auto& table, std::string_view text) {
        return put_blob<tag_v>(table, std::span<const std::byte>{ (const std::byte*)text.data(), text.size() });
    }

    // How many bytes are behind a reference, without reading them - useful for
    // sizing a buffer, and for deciding whether to read at all.
    template <tag_t tag_v>
    [[nodiscard]] std::size_t blob_size(const auto& table, blob_ref<tag_v> r) noexcept {
        return r ? table.length_of(r.id()) : 0;
    }

    // Into the caller's buffer, for a reader that has one already. Returns the
    // bytes written, 0 if the reference is empty or the buffer is too small -
    // a partial copy is never reported as success.
    template <tag_t tag_v>
    [[nodiscard]] std::size_t load_blob_into(const auto& table, blob_ref<tag_v> r, std::span<std::byte> out) {
        const std::size_t len = blob_size(table, r);
        if (!len || len > out.size()) return 0;
        return table.read_raw(r.id(), out.subspan(0, len)) ? len : 0;
    }

    template <tag_t tag_v>
    [[nodiscard]] std::string load_text(const auto& table, blob_ref<tag_v> r) {
        std::string out(blob_size(table, r), '\0');
        if (out.empty()) return out;
        if (!table.read_raw(r.id(), std::span<std::byte>{ (std::byte*)out.data(), out.size() })) out.clear();
        return out;
    }

    // Replaces the text behind a field: writes a new blob and hands back the
    // reference to store. The old record stays until compaction, exactly as an
    // overwritten field's old bytes do - this is an append-only log, and a blob
    // is not an exception to that.
    template <tag_t tag_v>
    [[nodiscard]] blob_ref<tag_v> replace_text(auto& table, blob_ref<tag_v> previous, std::string_view text) {
        (void)previous;   // named to make the caller's intent readable at the call site
        return put_text<tag_v>(table, text);
    }

    template <typename T> struct is_blob_ref : std::false_type {};
    template <tag_t tag_v> struct is_blob_ref<blob_ref<tag_v>> : std::true_type {};
    template <typename T> inline constexpr bool is_blob_ref_v = is_blob_ref<T>::value;

} // namespace storage
