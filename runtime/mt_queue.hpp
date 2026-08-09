#pragma once
// mt_queue.hpp - thread-safe queue for a genuine cross-OS-thread boundary
// (a fiber-scheduler thread handing work to/from a dedicated blocking-op
// thread). Different from protocol_endpoint's own incoming_queue/
// outgoing_queues in the io project: those are plain unsynchronized
// containers, correct precisely because nothing outside their single
// event-loop thread ever touches them. This queue is for the boundary
// where that stops being true.
//
// pop()/pop_batch() are non-blocking - call them from INSIDE a fiber. A
// fiber must never condvar-wait: that blocks the whole OS thread and
// defeats the entire point of cooperative yield(). pop_blocking() is for a
// dedicated OS thread that has nothing better to do than genuinely sleep
// until work exists.
//
// push_batch()/pop_batch() exist because a single push()/pop() pair pays a
// full lock (and, for push(), a condvar wake syscall) per item - fine for
// one-off items, wasteful once a producer already has a whole batch ready,
// which is exactly this queue's intended shape: a fiber drains its own
// inbound queue in one pass and hands the dedicated thread everything it
// collected in a single call. Measured against a hand-rolled SPSC ring:
// the wake syscall, not the mutex itself, is what actually dominates
// per-item cost under load (a "can genuinely sleep" ring buffer measured
// no faster than this queue - cutting notify_one() from N calls to 1 per
// batch is the real win available here, not switching data structures).

#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <vector>
#include <cstddef>

namespace mt {

    template <typename T>
    class queue {
    public:
        void push(T item) {
            { std::lock_guard lock(mutex); items.push_back(std::move(item)); }
            condvar.notify_one();
        }

        // Locks once for the whole batch and wakes the consumer once, not
        // once per item - see the file header.
        void push_batch(std::vector<T>&& batch) {
            if (batch.empty()) return;
            {
                std::lock_guard lock(mutex);
                for (auto& item : batch) items.push_back(std::move(item));
            }
            condvar.notify_one();
        }

        // non-blocking - call from INSIDE a fiber
        std::optional<T> pop() {
            std::lock_guard lock(mutex);
            if (items.empty()) return std::nullopt;
            T item = std::move(items.front());
            items.pop_front();
            return item;
        }

        // Drains up to max_count items in one locked section - the
        // fiber-side counterpart to push_batch(): one lock acquisition for
        // a whole batch instead of one per item.
        std::vector<T> pop_batch(std::size_t max_count) {
            std::vector<T> out;
            std::lock_guard lock(mutex);
            while (!items.empty() && out.size() < max_count) {
                out.push_back(std::move(items.front()));
                items.pop_front();
            }
            return out;
        }

        // blocking - for a dedicated OS thread: no reason to busy-loop
        // when it can genuinely sleep until work exists
        std::optional<T> pop_blocking() {
            std::unique_lock lock(mutex);
            condvar.wait(lock, [&] { return !items.empty() || stopped; });
            if (items.empty()) return std::nullopt; // woken by stop()
            T item = std::move(items.front());
            items.pop_front();
            return item;
        }

        void stop() {
            { std::lock_guard lock(mutex); stopped = true; }
            condvar.notify_all();
        }

    private:
        std::mutex mutex;
        std::condition_variable condvar;
        std::deque<T> items;
        bool stopped = false;
    };

} // namespace mt
