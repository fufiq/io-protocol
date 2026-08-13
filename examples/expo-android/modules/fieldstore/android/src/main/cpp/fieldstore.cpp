// fieldstore.cpp - the whole native side of the binding.
//
// THE BOUNDARY IS A BATCH, and every signature here is shaped by that. A query
// descriptor goes down, one packed buffer of records comes up; there is no call
// that returns a single field, and adding one would be the mistake this design
// exists to avoid. An engine update costs about 20 ns and a crossing costs
// orders of magnitude more, so anything per-record would measure the bridge.
//
// It is plain JNI rather than JSI, deliberately. JSI is what you reach for when
// the crossing is on the hot path; here the crossing happens once per batch of
// hundreds, so its cost is amortised into nothing and JSI's fragility buys
// nothing back. The one place it would matter - handing JS a buffer without a
// copy - is unavailable to this engine anyway: a field-indexed record does not
// exist contiguously anywhere, so it must be assembled into a buffer regardless
// (FIELD_STORAGE.md §5).
//
// Records cross as raw bytes in the layout schema.hpp asserts, and TypeScript
// decodes them with the offsets generated from the same schema. If the two ever
// disagree, schema.hpp fails to compile.

#include "schema.hpp"

#include <jni.h>
#include <memory>
#include <string>
#include <vector>

namespace {

    // Sized for a phone rather than a server: 64 MB of log, 100k ids at 16
    // bytes each plus 50k override nodes is about 2.8 MB of index, and the
    // index is the part that must stay resident (FIELD_STORAGE.md §12).
    using spec = storage::field_spec<
        "fieldstore.log", 64ull << 20,
        "fieldstore.idx", 100'000, 50'000,
        8, sizeof(wire::device),
        "fieldstore.ckp">;

    using table_t = storage::field_storage<spec>;

    std::unique_ptr<table_t> table;

    std::string to_utf8(JNIEnv* env, jstring s) {
        const char* raw = env->GetStringUTFChars(s, nullptr);
        std::string out = raw ? raw : "";
        if (raw) env->ReleaseStringUTFChars(s, raw);
        return out;
    }

} // namespace

extern "C" {

// The directory is only known at run time - it is the app's sandbox - which is
// the case storage_paths exists for.
JNIEXPORT jboolean JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeOpen(JNIEnv* env, jobject, jstring dir) {
    table = std::make_unique<table_t>(storage::storage_paths::under<spec>(to_utf8(env, dir)));
    if (!table->ok()) { table.reset(); return JNI_FALSE; }
    // Falls back to a full replay by itself when there is no usable checkpoint,
    // so this one call covers both a first launch and every one after it.
    table->restart(spec::capacity);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeClose(JNIEnv*, jobject) {
    if (table) table->sync();
    table.reset();
}

// One call, N records. `packed` is count * sizeof(device) bytes in the layout
// schema.hpp asserts; the ids come back in the same order.
JNIEXPORT jlongArray JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeCreateMany(JNIEnv* env, jobject, jbyteArray packed) {
    if (!table) return nullptr;
    const jsize bytes = env->GetArrayLength(packed);
    const jsize count = bytes / (jsize)sizeof(wire::device);
    if (count <= 0) return env->NewLongArray(0);

    std::vector<jbyte> buf((std::size_t)bytes);
    env->GetByteArrayRegion(packed, 0, bytes, buf.data());

    std::vector<jlong> ids((std::size_t)count);
    for (jsize i = 0; i < count; ++i) {
        wire::device d{};
        std::memcpy(&d, buf.data() + (std::size_t)i * sizeof(wire::device), sizeof(d));
        ids[(std::size_t)i] = (jlong)table->create(d);
    }
    jlongArray out = env->NewLongArray(count);
    env->SetLongArrayRegion(out, 0, count, ids.data());
    return out;
}

// A FIELD update, which is the entire premise: eight bytes reach the log, not
// seventy-two. The offset comes from the generated schema, so JS and C++ are
// naming the same field by construction.
//
// patch() rather than update<&device::field> because the offset is data here,
// not a pointer-to-member - which means it takes the checked path, and a diff
// that would reach past the end of the record is refused rather than applied.
JNIEXPORT void JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeUpdateU64(JNIEnv* env, jobject,
                                                             jlongArray ids, jint offset, jlongArray values) {
    if (!table) return;
    const jsize n = env->GetArrayLength(ids);
    if (n == 0 || env->GetArrayLength(values) != n) return;

    std::vector<jlong> id_buf((std::size_t)n), val_buf((std::size_t)n);
    env->GetLongArrayRegion(ids, 0, n, id_buf.data());
    env->GetLongArrayRegion(values, 0, n, val_buf.data());

    for (jsize i = 0; i < n; ++i) {
        const uint64_t v = (uint64_t)val_buf[(std::size_t)i];
        table->patch((storage::record_id)id_buf[(std::size_t)i], (uint32_t)offset,
                     std::span<const std::byte>{ (const std::byte*)&v, sizeof(v) });
    }
}

// The read half of the boundary: N ids down, one packed buffer up. Assembled
// straight into the buffer JS will read, which is the one copy assemble() has
// to make anyway.
JNIEXPORT jbyteArray JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeReadMany(JNIEnv* env, jobject, jlongArray ids) {
    if (!table) return nullptr;
    const jsize n = env->GetArrayLength(ids);
    std::vector<jlong> id_buf((std::size_t)n);
    env->GetLongArrayRegion(ids, 0, n, id_buf.data());

    std::vector<storage::record_id> want;
    want.reserve((std::size_t)n);
    for (jsize i = 0; i < n; ++i) want.push_back((storage::record_id)id_buf[(std::size_t)i]);

    std::vector<std::byte> out(want.size() * sizeof(wire::device));
    storage::prefetch_gather gather(64);
    gather.read_many(*table, want, out.data(), sizeof(wire::device));

    jbyteArray result = env->NewByteArray((jsize)out.size());
    env->SetByteArrayRegion(result, 0, (jsize)out.size(), (const jbyte*)out.data());
    return result;
}

// Everything alive, for a list view - the same packed buffer, ids alongside.
JNIEXPORT jbyteArray JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeReadAll(JNIEnv* env, jobject, jint limit) {
    if (!table) return nullptr;
    std::vector<storage::record_id> want;
    table->for_each<wire::device>([&](storage::record_id id, const wire::device&) {
        if ((jint)want.size() < limit) want.push_back(id);
    });
    std::vector<std::byte> out(want.size() * sizeof(wire::device));
    storage::prefetch_gather gather(64);
    gather.read_many(*table, want, out.data(), sizeof(wire::device));

    jbyteArray result = env->NewByteArray((jsize)out.size());
    env->SetByteArrayRegion(result, 0, (jsize)out.size(), (const jbyte*)out.data());
    return result;
}

// The durability barrier. Exposed rather than called automatically because
// where to put it is the caller's decision and getting it wrong is expensive in
// both directions (FIELD_STORAGE.md §8).
JNIEXPORT void JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeSync(JNIEnv*, jobject) {
    if (table) table->sync();
}

JNIEXPORT jboolean JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeCheckpoint(JNIEnv*, jobject) {
    return table && table->checkpoint() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlongArray JNICALL
Java_expo_modules_fieldstore_FieldStoreModule_nativeStats(JNIEnv* env, jobject) {
    jlong s[5] = {};
    if (table) {
        std::size_t live = 0;
        table->for_each_raw([&](auto) { ++live; });
        s[0] = (jlong)live;
        s[1] = (jlong)table->current_offset();
        s[2] = (jlong)table->live_bytes();
        s[3] = (jlong)table->overrides_used();
        s[4] = (jlong)storage::platform::page_size();
    }
    jlongArray out = env->NewLongArray(5);
    env->SetLongArrayRegion(out, 0, 5, s);
    return out;
}

} // extern "C"
