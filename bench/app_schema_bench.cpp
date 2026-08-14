// app_schema_bench.cpp - what the declared index layer costs, in numbers.
//
// Two questions, both of which were being answered with "some" until now.
//
//   1. What does maintaining the indexes cost on the write path? Measured as
//      the same writes with and without them, one table each.
//
//   2. What does a restart cost? There are four paths, and they span five
//      orders of magnitude:
//
//        recover()   replays the whole log; the observer sees every record, so
//                    the indexes are built as a side effect of rebuilding the
//                    engine. "Nothing extra to pay" was the guess going in, and
//                    it is wrong - they cost about as much again as the engine.
//        restart()   restores from a checkpoint and SKIPS the replay, so the
//        (rebuilt)   engine costs nothing and the indexes cost one pass over
//                    the live records - the only thing left to pay for.
//        restart()   the same, with the arena file the indexes live in still
//        (inherited) stamped for this log. Nothing is rebuilt: the file IS the
//                    index. This is what the last cell measures.
//
// WHAT THIS FOUND, since that is the reason to keep a benchmark rather than an
// estimate.
//
//   flat_ordered_map was quadratic. At 200,000 products the indexed fill was
//   +1115% and a replay +3296%, both growing with the square of the population:
//   inserting into the middle of a sorted vector moved n²/2 elements, 320 GB of
//   memmove for one fill. The class said "an insert into the middle moves the
//   tail", which was honest and which nobody would have turned into that
//   number. It batches inserts now, and the cells are flat.
//
//   The arena was 3x too big. Every index took the set's capacity, so the index
//   over 500,000 customers was sized for 4,000,000, and the key tables of the
//   multi indexes were sized for entries rather than keys: 884 MB against a
//   215 MB log. Per-index capacities brought it to 288 MB and took the indexed
//   replay from +323% to +106% with it - most of that cell was clearing arena
//   nobody was going to use.
//
// Each phase gets its own table object and its own timer, and the fill cells run
// `reps` times so a single unlucky run cannot be the number. The population is
// sized so that every cell takes long enough to mean something - at the earlier
// 370k records the whole thing finished in 12 ms and the run-to-run spread was
// larger than the effect being measured.

#include "app_schema.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

    constexpr std::size_t log_bytes = 2048ull << 20;
    constexpr std::size_t max_ids = 8'000'000;

    using spec = storage::field_spec<"as.log", log_bytes, "as.idx", max_ids, 4'000'000,
                                     8, 64, "as.ckp">;
    using indexed = storage::field_storage<spec, app2::schema_indexes>;
    using plain = storage::field_storage<spec>;

    void wipe() {
        std::remove(spec::path.value);
        std::remove(spec::index_path);
        std::remove(spec::checkpoint_path);
        std::remove((std::string(spec::index_path) + ".sx").c_str());
    }

    struct clock_mark {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        double seconds() const {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
    };

    struct xorshift {
        uint64_t s;
        explicit xorshift(uint64_t seed) : s(seed) {}
        uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        uint64_t range(uint64_t n) { return next() % n; }
    };

    // The same population either way, so the two write cells differ only in
    // whether an observer is attached.
    template <typename table>
    void populate(table& t, std::size_t customers, std::size_t products, std::size_t orders) {
        std::vector<storage::ref<app2::customer>> buyers;
        std::vector<storage::ref<app2::product>> items;
        buyers.reserve(customers);
        items.reserve(products);

        for (std::size_t i = 0; i < customers; ++i)
            buyers.push_back(t.create_ref(app2::customer{ 1'000'000 + i, (int64_t)(i * 7), 1, 0 }));
        for (std::size_t i = 0; i < products; ++i)
            items.push_back(t.create_ref(app2::product{ 5'000'000 + i, (int64_t)(i % 1000), 100 }));

        xorshift rng(12345);
        for (std::size_t i = 0; i < orders; ++i) {
            const auto buyer = buyers[rng.range(customers)];
            const auto o = t.create_ref(app2::order{ buyer, 0, 0, 0 });
            const int lines = 1 + (int)rng.range(3);
            int64_t total = 0;
            for (int l = 0; l < lines; ++l) {
                const auto item = items[rng.range(products)];
                const int64_t qty = 1 + (int64_t)rng.range(5);
                t.create(app2::order_line{ o, item, qty, 10 });
                total += qty * 10;
            }
            t.template update<&app2::order::total>(o.id(), total);
        }
    }

} // namespace

int main(int argc, char** argv) {
    const std::size_t customers = argc > 1 ? (std::size_t)std::atoll(argv[1]) : 500'000;
    const std::size_t products = argc > 2 ? (std::size_t)std::atoll(argv[2]) : 200'000;
    const std::size_t orders = argc > 3 ? (std::size_t)std::atoll(argv[3]) : 1'000'000;
    const int reps = argc > 4 ? std::atoi(argv[4]) : 3;

    // The index set is sized in its own type (app_schema.hpp), because the
    // engine maps its arena while constructing itself - before any argv could
    // have been read. So this checks the population against that capacity
    // rather than sizing to it: undersizing does not corrupt anything, it
    // refuses inserts and counts them, but a partial index would make every
    // number below meaningless.
    if (customers > app2::max_customers || products > app2::max_products ||
        orders > app2::max_orders || orders * 3 > app2::max_lines) {
        std::printf("!! that population is past what app_schema.hpp declares "
                    "(%zu customers, %zu products, %zu orders, %zu lines)\n",
                    app2::max_customers, app2::max_products, app2::max_orders, app2::max_lines);
        return 1;
    }

    std::printf("# %zu customers, %zu products, %zu orders (~%zu lines), best of %d\n"
                "# index arena: %.0f MB\n\n",
                customers, products, orders, orders * 2, reps,
                app2::schema_indexes::arena_bytes() / 1048576.0);

    // ---- the write path, with and without the indexes -------------------
    double plain_fill = 1e9, indexed_fill = 1e9;
    std::size_t records = 0, log_used = 0, index_failures = 0;

    for (int r = 0; r < reps; ++r) {
        wipe();
        auto t = std::make_unique<plain>();
        if (!t->ok()) { std::printf("could not map\n"); return 1; }
        clock_mark m;
        populate(*t, customers, products, orders);
        t->sync();
        const double took = m.seconds();
        if (took < plain_fill) plain_fill = took;
        if (r == reps - 1) {
            log_used = t->current_offset();
            t->for_each_raw([&](auto) { ++records; });
        }
    }

    for (int r = 0; r < reps; ++r) {
        wipe();
        auto t = std::make_unique<indexed>();
        if (!t->ok()) { std::printf("could not map\n"); return 1; }
        clock_mark m;
        populate(*t, customers, products, orders);
        t->sync();
        const double took = m.seconds();
        if (took < indexed_fill) indexed_fill = took;
        index_failures = t->observer_state().failed_inserts();
        if (r == reps - 1) t->checkpoint();
    }

    if (index_failures)
        std::printf("!! %zu index inserts did not fit - the numbers below are of a partial index\n\n",
                    index_failures);

    std::printf("%-34s %8.3f s  %10.0f rec/s\n", "fill, no indexes", plain_fill, (double)records / plain_fill);
    std::printf("%-34s %8.3f s  %10.0f rec/s   %+.0f%%\n", "fill, six indexes maintained",
                indexed_fill, (double)records / indexed_fill,
                100.0 * (indexed_fill - plain_fill) / plain_fill);
    std::printf("%-34s %8zu records, %.0f MB of log\n\n", "population", records, log_used / 1048576.0);

    // ---- the three restarts ---------------------------------------------
    double engine_only = 0, replay_indexed = 0, checkpoint_rebuild = 0, checkpoint_inherit = 0;
    std::size_t found = 0;
    const std::string arena_path = std::string(spec::index_path) + ".sx";
    {
        auto t = std::make_unique<plain>();
        clock_mark m;
        t->recover(log_bytes);
        engine_only = m.seconds();
    }
    // The arena is stamped by the fill's checkpoint and stays stamped until
    // something writes, so this cell has to come BEFORE the two that rebuild -
    // both of them leave the file valid but unvouched-for, which would turn
    // this measurement into a third rebuild without saying so.
    {
        auto t = std::make_unique<indexed>();
        clock_mark m;
        t->restart(log_bytes);
        checkpoint_inherit = m.seconds();
        if (!t->index_arena_inherited()) std::printf("!! the arena was refused; that cell is a rebuild\n");
        found = t->observer_state().template of<&app2::customer::email_hash>().size();
    }
    {
        auto t = std::make_unique<indexed>();
        clock_mark m;
        t->recover(log_bytes);
        replay_indexed = m.seconds();
        if (t->observer_state().template of<&app2::customer::email_hash>().size() != found)
            std::printf("!! the replayed index disagrees with the inherited one\n");
    }
    std::remove(arena_path.c_str());   // forces the rebuild the next cell prices
    {
        auto t = std::make_unique<indexed>();
        clock_mark m;
        t->restart(log_bytes);          // triggers on_restart -> rebuild from live records
        checkpoint_rebuild = m.seconds();
        if (!t->started_from_checkpoint()) std::printf("!! the checkpoint was not used; that cell is a replay\n");
        if (t->index_arena_inherited()) std::printf("!! the arena survived the wipe; that cell is not a rebuild\n");
        if (t->observer_state().template of<&app2::customer::email_hash>().size() != found)
            std::printf("!! the rebuilt index disagrees with the inherited one\n");
    }

    std::printf("%-34s %8.3f s   the engine alone, whole log replayed\n", "recover(), no indexes", engine_only);
    std::printf("%-34s %8.3f s   %+.0f%% over the engine alone\n",
                "recover(), six indexes", replay_indexed,
                100.0 * (replay_indexed - engine_only) / engine_only);
    std::printf("%-34s %8.3f s   no replay, no arena: one pass over LIVE records\n",
                "restart(), indexes rebuilt", checkpoint_rebuild);
    std::printf("%-34s %8.3f s   the indexes came back from their own file\n\n",
                "restart(), indexes inherited", checkpoint_inherit);

    std::printf("Reading it: six indexes cost %.0f%% on the write path and %.0f%% on a\n"
                "replay - roughly as much again as the engine itself, in both places, which\n"
                "is the honest answer to \"do they ride along for free\". That is why a\n"
                "checkpoint alone was worth only %.1fx on a restart: it removes the engine's\n"
                "replay, which was never the expensive half.\n\n"
                "Giving the indexes a file of their own removes the other half. %.3f s\n"
                "becomes %.3f s, %.0fx, and what is left is an mmap and a stamp comparison\n"
                "rather than a pass over %zu records. The stamp is the whole reason it is\n"
                "safe: it is cleared, durably, by the first write after a checkpoint, so a\n"
                "file that is behind the log is never the file that gets used.\n",
                100.0 * (indexed_fill - plain_fill) / plain_fill,
                100.0 * (replay_indexed - engine_only) / engine_only,
                replay_indexed / (checkpoint_rebuild > 0 ? checkpoint_rebuild : 1),
                checkpoint_rebuild, checkpoint_inherit,
                checkpoint_rebuild / (checkpoint_inherit > 0 ? checkpoint_inherit : 1e-9),
                records);

    wipe();
    return 0;
}
