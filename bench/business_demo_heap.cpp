// business_demo_heap.cpp - same scenario as business_demo.cpp
// (business logic on a fiber + a dedicated writer thread + queues),
// but on top of storage.hpp instead of record.hpp: storage::storage<user_spec> instead of
// storage::engine<user_spec> + eng.of<user>(). The substantive differences:
//   - no separate eng.of<user>(): storage IS the table.
//   - user does not keep its own id inside the payload - the index already knows it,
//     so duplicating it in the blob is pointless.
//   - user gained a tag (required by the tagged_aggregate concept), assigned
//     manually through the enum kind.
//
// Build: g++ -std=c++26 -O2 -pthread business_demo_heap.cpp -o business_demo_heap

#include "../runtime/fiber.hpp"
#include "../storage/storage.hpp"
#include "../runtime/mt_queue.hpp"

#include <cstdio>
#include <thread>
#include <atomic>
#include <unordered_map>

enum class kind : storage::tag_t { user = 1 };

struct user {
    static constexpr storage::tag_t tag = (storage::tag_t)kind::user;
    uint64_t balance;
};

using user_storage_spec = storage::storage_spec<storage::durability::local_sync, "users_demo.db">;

struct write_req { uint64_t req_id; storage::record_id user_id; uint64_t new_balance; };
struct write_ack { uint64_t req_id; };

mt::queue<write_req> requests;
mt::queue<write_ack> acks;
std::atomic<bool> running{true};

// --- writer thread: the only one that touches table after startup
// (storage<spec> is strictly single-threaded). ---
void writer_thread(storage::storage<user_storage_spec>& table) {
    while (running.load()) {
        auto r = requests.pop_blocking();
        if (!r) continue; // woken up by stop()
        table.update<&user::balance>(r->user_id, r->new_balance);
        table.sync(); // real msync() - here, not on the fiber thread
        acks.push(write_ack{ r->req_id });
    }
}

// --- business logic on a fiber: unchanged from the original ---
std::unordered_map<uint64_t, bool> completed;
uint64_t next_req_id = 1;

uint64_t submit(storage::record_id user_id, uint64_t new_balance) {
    uint64_t id = next_req_id++;
    requests.push(write_req{ id, user_id, new_balance });
    return id;
}

void await(uint64_t req_id) {
    while (!completed.contains(req_id)) fiber::yield();
    completed.erase(req_id);
}

void transfer(storage::record_id from_id, uint64_t current_from,
              storage::record_id to_id, uint64_t current_to, uint64_t amount) {
    printf("[transfer] debiting %llu from user#%llu\n", (unsigned long long)amount, (unsigned long long)from_id);
    auto req1 = submit(from_id, current_from - amount);
    await(req1);

    printf("[transfer] crediting %llu to user#%llu\n", (unsigned long long)amount, (unsigned long long)to_id);
    auto req2 = submit(to_id, current_to + amount);
    await(req2);

    printf("[transfer] done\n");
}

int main() {
    std::remove("users_demo.db");

    // local_sync - storage opens and maps the file itself, no shared_base needed
    // (unlike the old engine, which ALWAYS needed it, even
    // without a single shared/replicated table).
    storage::storage<user_storage_spec> table;

    auto alice = table.create(user{ 1000 });
    auto bob   = table.create(user{ 500 });
    table.sync();

    std::thread writer(writer_thread, std::ref(table));

    fiber::scheduler sched;
    sched.spawn([&] { transfer(alice, 1000, bob, 500, /*amount=*/300); });
    sched.spawn([&] {
        for (int i = 0; i < 3; ++i) {
            printf("[background] tick %d while transfer waits for the disk\n", i);
            fiber::yield();
        }
    });

    while (sched.has_pending()) {
        while (auto a = acks.pop()) completed[a->req_id] = true;
        sched.run_once();
    }

    running = false;
    requests.stop();
    writer.join(); // after join() it is safe to read table from the main thread again

    const user* a = table.get<user>(alice);
    const user* b = table.get<user>(bob);
    printf("\nresult: alice=%llu bob=%llu\n", (unsigned long long)a->balance, (unsigned long long)b->balance);
    return (a->balance == 700 && b->balance == 800) ? 0 : 1;
}
