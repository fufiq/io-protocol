#!/usr/bin/env python3
"""schema.json -> schema.hpp + schema.ts

One source of truth for the record layout. The generator computes offsets by C
alignment rules, then emits them TWICE: as static_asserts the C++ compiler must
agree with, and as constants the TS decoder reads. A layout the compiler lays
out differently is a build failure, not a wrong number at runtime.
"""

import json
import sys
from pathlib import Path

# type -> (c++ type, size, alignment, DataView getter, js type)
SCALARS = {
    "u64": ("uint64_t", 8, 8, "getBigUint64", "bigint"),
    "i64": ("int64_t",  8, 8, "getBigInt64",  "bigint"),
    "u32": ("uint32_t", 4, 4, "getUint32",    "number"),
    "i32": ("int32_t",  4, 4, "getInt32",     "number"),
    "u16": ("uint16_t", 2, 2, "getUint16",    "number"),
    "u8":  ("uint8_t",  1, 1, "getUint8",     "number"),
    "f64": ("double",   8, 8, "getFloat64",   "number"),
}


def align_up(n, a):
    return (n + a - 1) & ~(a - 1)


def lay_out(record):
    """Assign offsets by the same rules the C++ compiler uses.

    The engine's `aggregate` concept requires has_unique_object_representations,
    i.e. NO PADDING AT ALL - the checksum is taken over the bytes, and padding
    would make it depend on whatever was in the gap. So wherever alignment
    demands a gap, this emits an EXPLICIT pad member. The struct then has no
    padding bits, satisfies the concept, and the TS decoder simply skips it.
    """
    offset = 0
    struct_align = 1
    out = []
    pads = 0
    for f in record["fields"]:
        if f["type"] == "char":
            size, align = f["count"], 1
        else:
            _, size, align, _, _ = SCALARS[f["type"]]
        aligned = align_up(offset, align)
        if aligned != offset:
            out.append({"name": f"_pad{pads}", "type": "pad", "count": aligned - offset,
                        "offset": offset, "size": aligned - offset, "align": 1})
            pads += 1
            offset = aligned
        struct_align = max(struct_align, align)
        out.append(dict(f, offset=offset, size=size, align=align))
        offset += size

    total = align_up(offset, struct_align)
    if total != offset:
        out.append({"name": f"_pad{pads}", "type": "pad", "count": total - offset,
                    "offset": offset, "size": total - offset, "align": 1})
    return out, total, struct_align


def emit_hpp(schema, laid):
    L = []
    L.append("#pragma once")
    L.append("// GENERATED from schema.json by gen.py. Do not edit.")
    L.append("//")
    L.append("// Every offset here is asserted, not assumed. If the compiler pads this")
    L.append("// struct differently from what the TS decoder was generated against, the")
    L.append("// build stops here rather than handing JS bytes it will misread.")
    L.append("")
    L.append("#include <cstdint>")
    L.append("#include <cstddef>")
    L.append('#include "field_storage.hpp"')
    L.append("")
    L.append("namespace wire {")
    L.append("")
    L.append("    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,")
    L.append('                  "the TS decoder reads little-endian");')
    L.append("")

    for rec in schema["records"]:
        fields, size, salign = laid[rec["name"]]
        L.append(f"    struct {rec['name']} {{")
        L.append(f"        static constexpr storage::tag_t tag = {rec['tag']};")
        L.append("")
        for f in fields:
            if f["type"] == "pad":
                L.append(f"        char {f['name']}[{f['count']}];   // explicit: no padding bits allowed")
            elif f["type"] == "char":
                L.append(f"        char {f['name']}[{f['count']}];")
            else:
                L.append(f"        {SCALARS[f['type']][0]} {f['name']};")
        L.append("    };")
        L.append("")
        for f in fields:
            L.append(f"    static_assert(offsetof({rec['name']}, {f['name']}) == {f['offset']});")
        L.append(f"    static_assert(sizeof({rec['name']}) == {size});")
        # Deliberately no alignof assert. The WIRE contract is the offsets and
        # the size, and those are asserted above; the struct's own alignment is
        # ABI-dependent and not part of it. 32-bit x86 aligns uint64_t to 4
        # rather than 8, so asserting 8 failed an Android i686 build while every
        # offset and the size still matched exactly.
        L.append(f"    static_assert(storage::tagged_aggregate<{rec['name']}>);")
        L.append("")

    biggest = max(laid[r["name"]][1] for r in schema["records"])
    L.append(f"    inline constexpr std::size_t max_record_bytes = {biggest};")
    L.append("")
    L.append("} // namespace wire")
    return "\n".join(L) + "\n"


def emit_ts(schema, laid):
    L = []
    L.append("// GENERATED from schema.json by gen.py. Do not edit.")
    L.append("//")
    L.append("// Decodes the packed buffers the C++ side hands up. One crossing per batch;")
    L.append("// everything below this line is pure JS over an ArrayBuffer.")
    L.append("")
    L.append("const utf8 = new TextDecoder();")
    L.append("")
    L.append("function str(view: DataView, at: number, len: number): string {")
    L.append("    const bytes = new Uint8Array(view.buffer, view.byteOffset + at, len);")
    L.append("    const end = bytes.indexOf(0);")
    L.append("    return utf8.decode(end === -1 ? bytes : bytes.subarray(0, end));")
    L.append("}")
    L.append("")

    for rec in schema["records"]:
        fields, size, _ = laid[rec["name"]]
        name = rec["name"]
        cap = name[0].upper() + name[1:]

        visible = [f for f in fields if f["type"] != "pad"]

        L.append(f"export interface {cap} {{")
        for f in visible:
            ts = "string" if f["type"] == "char" else SCALARS[f["type"]][4]
            L.append(f"    {f['name']}: {ts};")
        L.append("}")
        L.append("")
        L.append(f"export const {cap}Layout = {{ tag: {rec['tag']}, size: {size} }} as const;")
        L.append("")
        L.append(f"export function decode{cap}(view: DataView, at: number): {cap} {{")
        L.append("    return {")
        for f in visible:
            if f["type"] == "char":
                L.append(f"        {f['name']}: str(view, at + {f['offset']}, {f['count']}),")
            else:
                getter = SCALARS[f["type"]][3]
                suffix = "" if f["size"] == 1 else ", true"
                L.append(f"        {f['name']}: view.{getter}(at + {f['offset']}{suffix}),")
        L.append("    };")
        L.append("}")
        L.append("")
        L.append(f"export function decode{cap}Batch(buffer: ArrayBuffer, count: number): {cap}[] {{")
        L.append("    const view = new DataView(buffer);")
        L.append(f"    const out: {cap}[] = new Array(count);")
        L.append(f"    for (let i = 0; i < count; i++) out[i] = decode{cap}(view, i * {size});")
        L.append("    return out;")
        L.append("}")
        L.append("")

    return "\n".join(L)


def main():
    here = Path(__file__).parent
    schema = json.loads((here / "schema.json").read_text())
    laid = {r["name"]: lay_out(r) for r in schema["records"]}

    root = here.parent
    (root / "modules/fieldstore/android/src/main/cpp/schema.hpp").write_text(emit_hpp(schema, laid))
    (root / "modules/fieldstore/src/schema.ts").write_text(emit_ts(schema, laid))

    for rec in schema["records"]:
        fields, size, salign = laid[rec["name"]]
        wasted = sum(f["size"] for f in fields if f["type"] == "pad")
        note = f"  <- {wasted} bytes wasted, reorder widest-first to reclaim" if wasted else ""
        print(f"{rec['name']:8} {size:3} bytes, align {salign}{note}")
        for f in fields:
            kind = f["type"] if f["type"] != "pad" else f"pad[{f['count']}]"
            print(f"    +{f['offset']:<4} {f['name']:14} {kind}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
