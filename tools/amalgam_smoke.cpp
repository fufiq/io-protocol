// amalgam_smoke.cpp - proves dist/field_storage.hpp works on its own.
//
// This file includes THE AMALGAMATION AND NOTHING ELSE from the project. No
// -I pointing at storage/, no relative paths reaching back into the tree. If
// the generator ever drops a file, mangles an include guard or leaves a
// dangling relative include, this stops compiling - which is the only way the
// single-file claim means anything.
//
// It exercises one of each kind of thing rather than duplicating the test
// suite: a compile-time spec and a run-time one, create/read/update/destroy,
// the observer, a durability barrier, checkpoint and restart, and a batched
// read through prefetch_gather. The suite proper still runs against the
// sources; this asks a narrower question - is the generated file whole?

#include "field_storage.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

    int failures = 0;
    void check(bool ok, const char* what) {
        std::printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }

    struct point {
        static constexpr storage::tag_t tag = 1;
        uint64_t id, x, y, z;
    };

    // An observer, because index maintenance below apply() is half of what the
    // engine is for and it would be easy for an amalgamation to lose.
    struct counting_observer {
        std::size_t applied = 0;
        void on_record(auto&, const storage::record_header&, const std::byte*) { ++applied; }
    };

    using spec = storage::field_spec<"amalgam.log", 32ull << 20,
                                     "amalgam.idx", 100'000, 50'000,
                                     8, sizeof(point), "amalgam.ckp">;
    using table_t = storage::field_storage<spec, counting_observer>;

    void remove_files(const std::string& dir) {
        const std::string prefix = dir.empty() ? std::string{} : dir + "/";
        std::remove((prefix + "amalgam.log").c_str());
        std::remove((prefix + "amalgam.idx").c_str());
        std::remove((prefix + "amalgam.ckp").c_str());
        if (!dir.empty()) ::rmdir(dir.c_str());
    }

} // namespace

int main() {
    remove_files("");

    std::vector<storage::record_id> ids;
    {
        table_t t;
        check(t.ok(), "open with the spec's own paths");
        if (!t.ok()) return 1;

        for (uint64_t i = 1; i <= 5000; ++i)
            ids.push_back(t.create(point{ i, i * 10, i * 100, i * 1000 }));

        // A field update writes the field, not the record - the whole premise.
        for (storage::record_id id : ids) t.update<&point::y>(id, 7);
        t.sync();

        auto got = t.get<point>(ids[42]);
        check(got && got->y == 7 && got->x == 43 * 10, "update one field, read the record back");
        check(t.observer_state().applied == 10000, "the observer saw every create and update");

        t.destroy(ids[0]);
        check(!t.alive(ids[0]), "destroy");

        // Batched read, which is the other supported header folded in here.
        std::vector<point> rows(64);
        storage::prefetch_gather gather(16);
        const auto r = gather.read_many(t, std::span<const storage::record_id>{ ids.data() + 100, 64 },
                                        (std::byte*)rows.data(), sizeof(point));
        check(r.records == 64 && rows[0].x == 101 * 10, "prefetch_gather read_many");

        check(t.checkpoint(), "checkpoint");
    }

    {
        table_t t;
        check(t.ok(), "reopen");
        t.restart(32ull << 20);
        check(t.started_from_checkpoint(), "restart came from the checkpoint");
        auto got = t.get<point>(ids[42]);
        check(got && got->y == 7, "the record survived the restart");
    }
    remove_files("");

    // Run-time paths, which is what a sandboxed process needs.
    {
        const std::string dir = "amalgam_dir";
        ::mkdir(dir.c_str(), 0755);
        table_t t(storage::storage_paths::under<spec>(dir));
        check(t.ok(), "open under a run-time directory");
        const auto id = t.create(point{ 1, 2, 3, 4 });
        auto got = t.get<point>(id);
        check(got && got->z == 4, "read back from the relocated files");
        struct ::stat st {};
        check(::stat("amalgam.log", &st) != 0, "nothing was created at the compile-time path");
        remove_files(dir);
    }

    std::printf("\n%s\n", failures == 0 ? "the amalgamated header is whole" : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
