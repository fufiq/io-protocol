// stress_heap.cpp - the analogue of stress.cpp, but on storage.hpp instead of record.hpp:
// same phase structure (A/B/C/recovery/D/E), plus phase F - aimed at
// background_compact() (variant 2) under concurrent load, which the
// original did not have (compaction was a separate topic there).
//
// capacity/target are reduced relative to the original (8GB/5GB) - otherwise
// the comparison burns a lot of time/space for the same conclusion;
// the relative phase structure and load pattern are unchanged, so
// ops/sec between the two binaries are directly comparable.
//
// Main difference from the original: no separate eng.of<T>() -
// storage<spec> is the table itself; T is never registered up front,
// only at the call site; id is not duplicated inside the payload (see app::user/
// app::account/profile - no id field, unlike the original).
//
// Build: g++ -std=c++26 -O3 -march=native -pthread stress_heap.cpp -o stress_heap

#include "../storage/storage.hpp"
#include "../runtime/mt_queue.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <chrono>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace app {
    // a single enum for the whole project - the only place where
    // tags are assigned; the engine knows nothing about it.
    enum class kind : storage::tag_t { user = 1, account = 2, profile = 3 };

    // id is not stored in the payload - the index already knows it, so duplicating it in the blob
    // is pointless (unlike the original record.hpp demo).
    struct user {
        static constexpr storage::tag_t tag = (storage::tag_t)kind::user;
        uint64_t balance;
        uint32_t age;
        char     name[100];
    };
    static_assert(sizeof(user) == 112, "user layout drifted");
    static_assert(storage::tagged_aggregate<user>);

    struct account {
        static constexpr storage::tag_t tag = (storage::tag_t)kind::account;
        uint64_t balance;
    };
    static_assert(storage::tagged_aggregate<account>);
}

using account_storage_spec = storage::storage_spec<storage::durability::local_sync, "stress_heap_accounts.db", 64ull * 1024 * 1024>;

namespace {

    constexpr std::size_t capacity    = 512ull * 1024 * 1024; // was 8GB in the original
    constexpr std::size_t target      = 200ull * 1024 * 1024; // was 5GB in the original
    constexpr int         n_slots     = 10;
    constexpr std::size_t n_transfers = 10'000;

    struct xor_shift64 {
        uint64_t state;
        explicit xor_shift64(uint64_t seed) noexcept : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
        uint64_t next() noexcept {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }
        uint64_t range(uint64_t lo, uint64_t hi) noexcept { return lo + next() % (hi - lo + 1); }
    };

    std::byte* open_mmap(const char* path, int& out_fd, std::size_t cap) {
        int fd = ::open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { std::perror("open"); std::exit(1); }

        struct stat st{};
        ::fstat(fd, &st);
        if ((std::size_t)st.st_size < cap) {
            int rc = ::posix_fallocate(fd, 0, cap);
            if (rc != 0) { std::fprintf(stderr, "posix_fallocate: %d\n", rc); std::exit(1); }
        }

        void* mem = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem == MAP_FAILED) { std::perror("mmap"); std::exit(1); }

        out_fd = fd;
        return (std::byte*)(mem);
    }

    struct expected_slot {
        bool alive = false;
        storage::record_id id = 0;
        uint64_t balance = 0;
        uint32_t age = 0;
        char name[100] = {};
    };

    constexpr const char* names[] = {
        "viktor", "roman", "ivan", "anna", "olga",
        "petr", "sergey", "maria", "alex", "dmitry"
    };

    double elapsed(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }

} // namespace

// =====================================================================
// Phase D: transfers on the local_sync heap through a writer thread. Compaction is
// variant 1 (blocking), run synchronously from the post_batch hook of the
// writer thread itself, exactly as in the original.
// =====================================================================
namespace transfer_bench {

    struct transfer_req { storage::record_id from; uint64_t new_from_balance; storage::record_id to; uint64_t new_to_balance; };

    mt::queue<transfer_req> requests;
    std::atomic<std::size_t> committed{0};
    std::atomic<bool> running{true};
    std::mutex table_mu;

    void maybe_compact(storage::storage<account_storage_spec>& table) {
        constexpr double garbage_threshold = 0.5;
        if (table.garbage_ratio() < garbage_threshold) return;

        std::string live_path = std::string(account_storage_spec::path.value);
        std::string tmp_path = live_path + ".compact_tmp";
        std::string live_data_path = std::string(account_storage_spec::data_path);
        std::string tmp_data_path = live_data_path + ".compact_tmp";

        int tmp_fd, tmp_data_fd;
        std::byte* scratch = open_mmap(tmp_path.c_str(), tmp_fd, account_storage_spec::capacity);
        std::byte* scratch_data = open_mmap(tmp_data_path.c_str(), tmp_data_fd, account_storage_spec::capacity);

        table.compact(scratch, scratch_data);
        table.sync();

        if (::close(tmp_fd) != 0 || ::rename(tmp_path.c_str(), live_path.c_str()) != 0) {
            std::perror("compaction rename failed");
        }
        if (::close(tmp_data_fd) != 0 || ::rename(tmp_data_path.c_str(), live_data_path.c_str()) != 0) {
            std::perror("compaction data rename failed");
        }
    }

    void writer_thread(storage::storage<account_storage_spec>& table) {
        storage::batch_commit(table, requests, running, table_mu,
                              [](storage::storage<account_storage_spec>& t, const transfer_req& r) {
                                  t.update<&app::account::balance>(r.from, r.new_from_balance);
                                  t.update<&app::account::balance>(r.to, r.new_to_balance);
                                  committed.fetch_add(1, std::memory_order_relaxed);
                              },
                              maybe_compact);
    }

    void run(storage::storage<>& user_db, std::array<expected_slot, n_slots>& slots) {
        std::remove("stress_heap_accounts.db");

        storage::storage<account_storage_spec> table; // local_sync - opens the file itself

        std::array<storage::record_id, n_slots> ids{};
        std::array<uint64_t, n_slots> expected_balance{};
        for (int i = 0; i < n_slots; ++i) {
            ids[i] = table.create(app::account{ 1'000'000 });
            expected_balance[i] = 1'000'000;
        }
        table.sync();

        std::thread writer(writer_thread, std::ref(table));

        auto t0 = std::chrono::steady_clock::now();
        constexpr uint64_t amount = 100;
        for (std::size_t i = 0; i < n_transfers; ++i) {
            int from = (int)(i % n_slots);
            int to = (int)((i + 1) % n_slots);

            expected_balance[from] -= amount;
            expected_balance[to] += amount;

            requests.push(transfer_req{ ids[from], expected_balance[from], ids[to], expected_balance[to] });
        }
        auto t1 = std::chrono::steady_clock::now();
        double submit_secs = elapsed(t0, t1);
        std::printf("[phaseD:submit]  %zu transfers queued in %.4f sec (%.0f ops/sec - NOT durable, only freeze + enqueue)\n",
                    n_transfers, submit_secs, n_transfers / submit_secs);

        constexpr std::size_t safety_margin = 20ull * 1024 * 1024;
        xor_shift64 rng2(54321);
        auto last_sync_at = std::chrono::steady_clock::now();
        std::size_t unrelated_updates = 0;
        std::size_t k = 0;
        while (committed.load(std::memory_order_relaxed) < n_transfers) {
            if (user_db.current_offset() >= capacity - safety_margin) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            int slot = (int)(k % n_slots);
            auto& s = slots[slot];

            if ((k / n_slots) % 2 == 0) {
                uint64_t nb = rng2.range(0, 1'000'000);
                user_db.update<&app::user::balance>(s.id, nb);
                s.balance = nb;
            } else {
                uint32_t na = (uint32_t)rng2.range(18, 90);
                user_db.update<&app::user::age>(s.id, na);
                s.age = na;
            }
            ++unrelated_updates;
            ++k;

            auto now_check = std::chrono::steady_clock::now();
            if (unrelated_updates % 1'000'000 == 0 || elapsed(last_sync_at, now_check) >= 0.2) {
                user_db.sync();
                last_sync_at = now_check;
            }
        }
        auto t2 = std::chrono::steady_clock::now();
        double commit_secs = elapsed(t1, t2);
        double unrelated_secs = commit_secs;

        std::printf("[phaseD:unrelated] %zu UNRELATED user-heap updates IN PARALLEL with the transfer commit, %.4f sec (%.0f ops/sec)\n",
                    unrelated_updates, unrelated_secs, unrelated_updates / unrelated_secs);
        std::printf("[phaseD:commit]  %zu transfers durably committed (msync) in %.4f sec (%.0f ops/sec - REAL durable speed)\n",
                    n_transfers, commit_secs, n_transfers / commit_secs);
        std::printf("[phaseD:ratio]   submit is %.1fx faster than commit\n", commit_secs / submit_secs);

        running = false;
        requests.stop();
        writer.join();

        bool ok = true;
        for (int i = 0; i < n_slots; ++i) {
            const app::account* a = table.get<app::account>(ids[i]);
            bool match = a && a->balance == expected_balance[i];
            if (!match) {
                std::printf("[phaseD:verify] slot %d MISMATCH: expected=%llu got=%llu\n",
                            i, (unsigned long long)expected_balance[i],
                            a ? (unsigned long long)a->balance : 0);
                ok = false;
            }
        }
        std::printf(ok ? "[phaseD:verify] ALL %d ACCOUNTS OK\n" : "[phaseD:verify] FAILURES FOUND\n", n_slots);
        std::printf("[phaseD:compact] final garbage_ratio=%.4f (compaction fired at >=0.5 inside the writer)\n", table.garbage_ratio());
    }

} // namespace transfer_bench

// =====================================================================
// Phase E: recreate() - CREATE over an occupied id.
// =====================================================================
namespace recreate_bench {

    struct profile {
        static constexpr storage::tag_t tag = (storage::tag_t)app::kind::profile;
        char name[32];
        uint32_t version;
    };
    static_assert(storage::tagged_aggregate<profile>);

    struct expected_profile { char name[32] = {}; uint32_t version = 0; };

    constexpr int n_profiles = 5;
    constexpr int n_rounds = 2000;
    const std::size_t record_stride = storage::pad8(sizeof(storage::record_header) + sizeof(profile));

    void set_name(profile& p, const char* s) {
        std::memset(p.name, 0, sizeof(p.name));
        std::memcpy(p.name, s, std::strlen(s));
    }
    bool matches(const profile& p, const expected_profile& e) {
        return p.version == e.version && std::memcmp(p.name, e.name, sizeof(e.name)) == 0;
    }

} // namespace recreate_bench

using profile_local_storage_spec = storage::storage_spec<storage::durability::local_sync, "stress_heap_recreate_local.db", 4ull * 1024 * 1024>;

namespace recreate_bench {

    // ---- E1: local_sync, recreate() + compact() (variant 1) ---------------
    bool run_local() {
        std::remove("stress_heap_recreate_local.db");
        bool ok = true;

        storage::storage<profile_local_storage_spec> table;

        std::array<storage::record_id, n_profiles> ids{};
        std::array<expected_profile, n_profiles> expected{};
        const std::size_t one_size = sizeof(profile);
        const std::size_t expected_live = n_profiles * one_size;

        for (int i = 0; i < n_profiles; ++i) {
            storage::record_id id = (storage::record_id)(i + 1);
            profile p{}; set_name(p, names[i % 10]); p.version = 0;
            table.recreate(id, p); // recreate() on a fresh id must behave like create()
            ids[i] = id;
            std::memcpy(expected[i].name, p.name, sizeof(p.name));
            expected[i].version = 0;
        }
        if (table.live_bytes() != expected_live) {
            std::printf("[phaseE1:init] live_bytes MISMATCH: expected=%zu got=%zu\n", expected_live, table.live_bytes());
            ok = false;
        }

        for (int round = 0; round < n_rounds; ++round) {
            int slot = round % n_profiles;
            profile p{};
            set_name(p, names[(round + slot) % 10]);
            p.version = (uint32_t)(round + 1);

            table.recreate(ids[slot], p);
            std::memcpy(expected[slot].name, p.name, sizeof(p.name));
            expected[slot].version = p.version;

            if (table.live_bytes() != expected_live) {
                std::printf("[phaseE1:recreate] round %d: live_bytes DRIFT: expected=%zu got=%zu\n",
                            round, expected_live, table.live_bytes());
                ok = false;
                break;
            }
        }
        table.sync();

        std::size_t expected_offset = (std::size_t)(n_profiles + n_rounds) * record_stride;
        if (table.current_offset() != expected_offset) {
            std::printf("[phaseE1:offset] MISMATCH: expected=%zu got=%zu\n", expected_offset, table.current_offset());
            ok = false;
        }

        for (int i = 0; i < n_profiles; ++i) {
            const profile* p = table.get<profile>(ids[i]);
            if (!p || !matches(*p, expected[i])) {
                std::printf("[phaseE1:verify] slot %d MISMATCH after recreate rounds\n", i);
                ok = false;
            }
        }
        std::printf("[phaseE1:recreate] %d rounds ok, live_bytes=%zu offset=%zu garbage_ratio=%.4f\n",
                    n_rounds, table.live_bytes(), table.current_offset(), table.garbage_ratio());

        {
            std::remove("stress_heap_recreate_local.compact_tmp");
            std::remove("stress_heap_recreate_local.compact_tmp.data");
            int scratch_fd, scratch_data_fd;
            std::byte* scratch = open_mmap("stress_heap_recreate_local.compact_tmp", scratch_fd, profile_local_storage_spec::capacity);
            std::byte* scratch_data = open_mmap("stress_heap_recreate_local.compact_tmp.data", scratch_data_fd, profile_local_storage_spec::capacity);
            table.compact(scratch, scratch_data);

            if (table.live_bytes() != expected_live) {
                std::printf("[phaseE1:compact] live_bytes MISMATCH after compact(): expected=%zu got=%zu\n",
                            expected_live, table.live_bytes());
                ok = false;
            }
            double expected_floor = 1.0 - (double)one_size / (double)record_stride;
            if (std::abs(table.garbage_ratio() - expected_floor) > 1e-9) {
                std::printf("[phaseE1:compact] garbage_ratio OFF the floor: expected=%.6f got=%.6f\n",
                            expected_floor, table.garbage_ratio());
                ok = false;
            }
            for (int i = 0; i < n_profiles; ++i) {
                const profile* p = table.get<profile>(ids[i]);
                if (!p || !matches(*p, expected[i])) {
                    std::printf("[phaseE1:compact] slot %d MISMATCH after compact()\n", i);
                    ok = false;
                }
            }
            ::close(scratch_fd);
            std::printf("[phaseE1:compact] ok, live_bytes=%zu garbage_ratio=%.4f (floor=%.4f)\n",
                        table.live_bytes(), table.garbage_ratio(), expected_floor);
        }

        {
            std::size_t before = table.live_bytes();
            table.destroy(ids[0]);
            if (table.live_bytes() != before - one_size) {
                std::printf("[phaseE1:destroy] live_bytes MISMATCH: expected=%zu got=%zu\n",
                            before - one_size, table.live_bytes());
                ok = false;
            }
            if (table.get<profile>(ids[0]) != nullptr) {
                std::printf("[phaseE1:destroy] slot 0 is still alive after destroy()\n");
                ok = false;
            }
        }

        std::printf(ok ? "[phaseE1] ALL LOCAL_SYNC RECREATE CHECKS OK\n" : "[phaseE1] FAILURES FOUND\n");
        return ok;
    }

    // ---- E2: shared + recover() --------------------------------------------
    bool run_recover() {
        const char* path = "stress_heap_recreate_shared.storage";
        std::remove(path);
        constexpr std::size_t cap = 16ull * 1024 * 1024;

        std::array<storage::record_id, n_profiles> ids{};
        std::array<expected_profile, n_profiles> expected{};
        const std::size_t expected_live = n_profiles * sizeof(profile);
        bool ok = true;

        {
            int fd = -1;
            std::byte* base = open_mmap(path, fd, cap);
            storage::storage<> db(base); // default spec = shared

            for (int i = 0; i < n_profiles; ++i) {
                profile p{}; set_name(p, names[i % 10]); p.version = 0;
                auto id = db.create(p);
                ids[i] = id;
                std::memcpy(expected[i].name, p.name, sizeof(p.name));
                expected[i].version = 0;
            }

            for (int r = 0; r < n_rounds; ++r) {
                int slot = r % n_profiles;
                profile p{};
                set_name(p, names[(r + slot) % 10]);
                p.version = (uint32_t)(r + 1);
                db.recreate(ids[slot], p);
                std::memcpy(expected[slot].name, p.name, sizeof(p.name));
                expected[slot].version = p.version;
            }

            ::close(fd); // "crash"
        }

        {
            int fd = -1;
            std::byte* base = open_mmap(path, fd, cap);
            storage::storage<> db(base);
            struct stat st{};
            ::fstat(fd, &st);
            db.recover((std::size_t)st.st_size);

            if (db.live_bytes() != expected_live) {
                std::printf("[phaseE2:recover] live_bytes MISMATCH: expected=%zu got=%zu\n",
                            expected_live, db.live_bytes());
                ok = false;
            }
            for (int i = 0; i < n_profiles; ++i) {
                const profile* p = db.get<profile>(ids[i]);
                if (!p || !matches(*p, expected[i])) {
                    std::printf("[phaseE2:recover] slot %d MISMATCH after recover()\n", i);
                    ok = false;
                }
            }
            ::close(fd);
        }

        std::printf(ok ? "[phaseE2] recreate()+recover() OK: live_bytes is correct after %d rounds on id\n" : "[phaseE2] FAILURES FOUND\n", n_rounds);
        return ok;
    }

    bool run_all() {
        bool a = run_local();
        bool b = run_recover();
        return a && b;
    }

} // namespace recreate_bench

// =====================================================================
// Phase F: background_compact() (variant 2) under concurrent load -
// the writer (through batch_commit + mutex) and the compactor on a separate thread,
// both on the same mutex. The original had no such scenario (compaction was
// a separate topic) - added specifically so that both compaction variants
// are exercised under load in practice, not only in the E1 unit check.
// =====================================================================
namespace background_bench {

    using spec = storage::storage_spec<storage::durability::local_sync, "stress_heap_bg_compact.db", 32ull * 1024 * 1024>;

    struct bump_req { storage::record_id id; uint64_t new_balance; };

    mt::queue<bump_req> requests;
    std::atomic<bool> running{true};
    std::atomic<std::size_t> committed{0};
    std::mutex mu;

    constexpr int n_accounts = 20;
    constexpr std::size_t n_ops = 300'000;

    void writer_thread(storage::storage<spec>& table) {
        storage::batch_commit(table, requests, running, mu,
                              [](storage::storage<spec>& t, const bump_req& r) {
                                  t.update<&app::account::balance>(r.id, r.new_balance);
                                  committed.fetch_add(1, std::memory_order_relaxed);
                              },
                              [](storage::storage<spec>&) {}, // post_batch - the compactor runs on its own thread, not needed here
                              /*max_batch=*/500, std::chrono::milliseconds(2));
    }

    void run() {
        std::remove("stress_heap_bg_compact.db");
        storage::storage<spec> table;

        std::array<storage::record_id, n_accounts> ids{};
        std::array<uint64_t, n_accounts> expected{};
        for (int i = 0; i < n_accounts; ++i) {
            ids[i] = table.create(app::account{ 1'000'000 });
            expected[i] = 1'000'000;
        }
        table.sync();

        std::thread writer(writer_thread, std::ref(table));

        std::atomic<bool> compactor_running{true};
        std::atomic<std::size_t> compactions{0};
        std::thread compactor([&] {
            std::size_t last = 0;
            while (compactor_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                if (committed.load(std::memory_order_relaxed) - last > 30'000) {
                    storage::background_compact(table, mu, "stress_heap_bg_compact.db.tmp", "stress_heap_bg_compact.db", spec::capacity,
                                                "stress_heap_bg_compact.db.data.tmp", "stress_heap_bg_compact.db.data");
                    last = committed.load(std::memory_order_relaxed);
                    compactions.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        xor_shift64 rng(999);
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_ops; ++i) {
            int slot = (int)(i % n_accounts);
            uint64_t nb = rng.range(0, 10'000'000);
            expected[slot] = nb;
            requests.push(bump_req{ ids[slot], nb });
        }
        while (committed.load(std::memory_order_relaxed) < n_ops) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto t1 = std::chrono::steady_clock::now();

        running = false;
        requests.stop();
        writer.join();
        compactor_running = false;
        compactor.join();

        // final synchronous catch-up - guaranteed to reflect everything up to the last committed
        storage::background_compact(table, mu, "stress_heap_bg_compact.db.tmp", "stress_heap_bg_compact.db", spec::capacity,
                                                "stress_heap_bg_compact.db.data.tmp", "stress_heap_bg_compact.db.data");

        double secs = elapsed(t0, t1);
        std::printf("[phaseF:background_compact] %zu updates under %zu background compactions, %.4f sec (%.0f ops/sec, durable)\n",
                    n_ops, compactions.load(), secs, n_ops / secs);
        std::printf("[phaseF:background_compact] final garbage_ratio=%.4f (compaction fired every ~30k committed)\n",
                    table.garbage_ratio());

        bool ok = true;
        for (int i = 0; i < n_accounts; ++i) {
            const app::account* a = table.get<app::account>(ids[i]);
            if (!a || a->balance != expected[i]) {
                std::printf("[phaseF:verify] slot %d MISMATCH: expected=%llu got=%llu\n",
                            i, (unsigned long long)expected[i], a ? (unsigned long long)a->balance : 0);
                ok = false;
            }
        }
        std::printf(ok ? "[phaseF:verify] ALL %d ACCOUNTS OK UNDER CONCURRENT COMPACTION\n" : "[phaseF:verify] FAILURES FOUND\n", n_accounts);
    }

} // namespace background_bench

int main() {
    const char* path = "stress_heap.storage";
    std::remove(path);

    std::array<expected_slot, n_slots> slots{};
    xor_shift64 rng(12345);

    std::size_t creates = 0, updates = 0, destroys = 0;

    auto t_start = std::chrono::steady_clock::now();
    {
        int fd = -1;
        std::byte* base = open_mmap(path, fd, capacity);
        storage::storage<> db(base); // default spec = shared, T is never registered

        // ---- Phase A: create all 10 slots once ----------------------
        auto t_a0 = std::chrono::steady_clock::now();
        for (int slot = 0; slot < n_slots; ++slot) {
            app::user u{};
            u.balance = rng.range(0, 1'000'000);
            u.age = (uint32_t)rng.range(18, 90);
            std::memset(u.name, 0, sizeof(u.name));
            std::memcpy(u.name, names[slot], std::strlen(names[slot]));

            auto rid = db.create(u);
            auto& s = slots[slot];
            s = { true, rid, u.balance, u.age, {} };
            std::memcpy(s.name, u.name, sizeof(s.name));
            ++creates;
        }
        auto t_a1 = std::chrono::steady_clock::now();
        std::printf("[phaseA:create] %zu ops in %.4f sec\n", creates, elapsed(t_a0, t_a1));

        // ---- Phase B: pure updates, round-robin over slots and fields -------
        auto t_b0 = std::chrono::steady_clock::now();
        const std::size_t target_b = (std::size_t)(target * 0.7);
        std::size_t i = 0;
        while (db.current_offset() < target_b) {
            int slot = (int)(i % n_slots);
            auto& s = slots[slot];

            if ((i / n_slots) % 2 == 0) {
                uint64_t nb = rng.range(0, 1'000'000);
                db.update<&app::user::balance>(s.id, nb);
                s.balance = nb;
            } else {
                uint32_t na = (uint32_t)rng.range(18, 90);
                db.update<&app::user::age>(s.id, na);
                s.age = na;
            }
            ++updates;
            ++i;
        }
        auto t_b1 = std::chrono::steady_clock::now();
        std::printf("[phaseB:update] %zu ops in %.4f sec (%.0f ops/sec)\n",
                    updates, elapsed(t_b0, t_b1), updates / elapsed(t_b0, t_b1));

        // ---- Phase C: destroy+recreate, also round-robin -------------------
        auto t_c0 = std::chrono::steady_clock::now();
        std::size_t c_ops = 0;
        std::size_t j = 0;
        while (db.current_offset() < target) {
            int slot = (int)(j % n_slots);
            auto& s = slots[slot];

            db.destroy(s.id);
            ++destroys;

            app::user u{};
            u.balance = rng.range(0, 1'000'000);
            u.age = (uint32_t)rng.range(18, 90);
            std::memset(u.name, 0, sizeof(u.name));
            std::memcpy(u.name, names[slot], std::strlen(names[slot]));

            auto rid = db.create(u);
            s = { true, rid, u.balance, u.age, {} };
            std::memcpy(s.name, u.name, sizeof(s.name));
            ++creates;

            c_ops += 2;
            ++j;
        }
        auto t_c1 = std::chrono::steady_clock::now();
        std::printf("[phaseC:destroy+recreate] %zu ops in %.4f sec (%.0f ops/sec)\n",
                    c_ops, elapsed(t_c0, t_c1), c_ops / elapsed(t_c0, t_c1));

        std::printf("[phase1] total: create=%zu update=%zu destroy=%zu, file=%.2f MB\n",
                    creates, updates, destroys,
                    db.current_offset() / (1024.0 * 1024.0));

        ::close(fd); // "crash"
    }
    auto t_after_load = std::chrono::steady_clock::now();
    std::printf("[perf] total load phase: %.3f sec, %.1f MB/sec\n",
                elapsed(t_start, t_after_load),
                (target / (1024.0 * 1024.0)) / elapsed(t_start, t_after_load));

    // ---- Phase 2: recovery on the shared heap -------------------------------
    auto t_recover_start = std::chrono::steady_clock::now();
    bool phase1_ok = true;
    {
        int fd = -1;
        std::byte* base = open_mmap(path, fd, capacity);
        storage::storage<> db(base);

        struct stat st{};
        ::fstat(fd, &st);

        std::size_t restored_offset = db.recover((std::size_t)st.st_size);

        auto t_recover_end = std::chrono::steady_clock::now();
        double recover_secs = elapsed(t_recover_start, t_recover_end);

        std::printf("[perf] recovery: %.3f sec, restored_offset=%zu (%.2f MB), %.1f MB/sec\n",
                    recover_secs, restored_offset,
                    restored_offset / (1024.0 * 1024.0),
                    (restored_offset / (1024.0 * 1024.0)) / recover_secs);

        for (int slot = 0; slot < n_slots; ++slot) {
            const auto& s = slots[slot];
            if (!s.alive) continue;

            const app::user* u = db.get<app::user>(s.id);
            if (!u) {
                std::printf("[verify] slot %d (id=%llu): MISSING after recovery - FAIL\n", slot, (unsigned long long)s.id);
                phase1_ok = false;
                continue;
            }
            bool match = (u->balance == s.balance) && (u->age == s.age) &&
                         (std::memcmp(u->name, s.name, sizeof(s.name)) == 0);
            if (!match) {
                std::printf("[verify] slot %d (id=%llu): MISMATCH - FAIL\n", slot, (unsigned long long)s.id);
                phase1_ok = false;
            }
        }
        std::printf(phase1_ok ? "[verify] ALL %d ALIVE SLOTS OK\n" : "[verify] FAILURES FOUND\n", n_slots);

        std::printf("\n=== Phase D: transfers through local_sync + writer thread (blocking compact()) ===\n");
        transfer_bench::run(db, slots);

        ::close(fd);
    }

    std::printf("\n=== Phase E: recreate() - CREATE over an occupied id ===\n");
    bool phaseE_ok = recreate_bench::run_all();

    std::printf("\n=== Phase F: background_compact() under concurrent load ===\n");
    background_bench::run();

    return (phase1_ok && phaseE_ok) ? 0 : 1;
}
