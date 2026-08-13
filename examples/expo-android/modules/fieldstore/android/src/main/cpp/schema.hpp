#pragma once
// GENERATED from schema.json by gen.py. Do not edit.
//
// Every offset here is asserted, not assumed. If the compiler pads this
// struct differently from what the TS decoder was generated against, the
// build stops here rather than handing JS bytes it will misread.

#include <cstdint>
#include <cstddef>
#include "field_storage.hpp"

namespace wire {

    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                  "the TS decoder reads little-endian");

    struct device {
        static constexpr storage::tag_t tag = 1;

        uint64_t serial;
        uint64_t last_seen_ms;
        int64_t credit;
        uint32_t firmware;
        uint8_t status;
        char name[40];
        char _pad0[3];   // explicit: no padding bits allowed
    };

    static_assert(offsetof(device, serial) == 0);
    static_assert(offsetof(device, last_seen_ms) == 8);
    static_assert(offsetof(device, credit) == 16);
    static_assert(offsetof(device, firmware) == 24);
    static_assert(offsetof(device, status) == 28);
    static_assert(offsetof(device, name) == 29);
    static_assert(offsetof(device, _pad0) == 69);
    static_assert(sizeof(device) == 72);
    static_assert(storage::tagged_aggregate<device>);

    inline constexpr std::size_t max_record_bytes = 72;

} // namespace wire
