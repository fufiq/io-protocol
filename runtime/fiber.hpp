#pragma once
// fiber.hpp - stackful cooperative fibers on ucontext (not C++ coroutines).
// Cooperative multitasking: an explicit yield() switches back to whoever
// called resume(); scheduler resumes each not-yet-finished fiber in a
// round-robin pass.
//
// Honest limitation, unchanged from before: a fiber does NOT turn a
// blocking syscall (msync, read, etc.) into a non-blocking one by itself -
// it only gives cooperative stack switching. Calling msync() directly
// inside a fiber still blocks the OS thread for the syscall's duration;
// explicit yield() before/after just lets you hand control to other fibers
// around it. Real async I/O still needs io_uring, or handing the blocking
// call to a dedicated OS thread (see mt_queue.hpp) and yielding until its
// result comes back.
//
// current is thread_local, not a single global: each OS thread runs its
// own independent fiber::scheduler (confirmed design - business logic gets
// one scheduler per OS thread, not one shared across all of them), and
// each scheduler's "which fiber is running right now" state must not be
// shared across threads, or two schedulers running concurrently would
// clobber the same pointer.
//
// ASan/UBSan note: raw ucontext switches are invisible to AddressSanitizer
// by default - it has no way to know the stack pointer just jumped
// somewhere else, which can produce spurious stack-related reports (or
// mask real ones) the moment fibers are exercised under -fsanitize=address.
// resume()/yield()/trampoline() call the sanitizer's own fiber-switch hooks
// (__sanitizer_start_switch_fiber/__sanitizer_finish_switch_fiber) to keep
// it informed, compiled to nothing when ASan isn't enabled. The exact
// calling convention here mirrors compiler-rt's own reference test for this
// API (compiler-rt/test/asan/TestCases/Linux/swapcontext_annotation.cpp) -
// this is a genuinely fiddly API to get right (even Boost.Context has had
// bugs from calling finish_switch_fiber at the wrong point relative to the
// context switch), so don't improvise the ordering without re-checking
// that reference.
//
// REQUIRED when running anything built on this under ASan:
// ASAN_OPTIONS=detect_stack_use_after_return=0
// Without it, ANY fiber whose fn (or something it calls) has a local array
// and then yields one or more call-frames deep will SEGV on the very first
// resume after that yield - confirmed empirically (gcc 14 and clang 20
// libasan, both identical), and confirmed NOT caused by anything on this
// file's side: the exact same crash happens whether the
// __sanitizer_start/finish_switch_fiber calls above are present or
// removed entirely, at 64KB or 4MB stack sizes, with or without
// recursion - it's specifically ASan's detect_stack_use_after_return
// "fake stack" (a heap-backed shadow of the real stack, used to catch
// use-after-return bugs) getting confused by a real stack pointer jumping
// to a different, unrelated memory region mid-function. This is NOT a
// theoretical caveat - it will not "usually be fine": any real
// business-logic fiber with a local array that yields inside a nested
// call needs this env var set, every time, under ASan. Turning that one
// feature off leaves the rest of ASan (heap/global overflow, use-after-
// free, all of UBSan) fully intact - only the stack-use-after-return
// checks are disabled, which fiber code was never able to rely on anyway.

#include <ucontext.h>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>
#include <exception>
#include <utility>
#include <stdexcept>
#include <sys/mman.h>

#if defined(__SANITIZE_ADDRESS__)
#define FIBER_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FIBER_HAS_ASAN 1
#endif
#endif

#if FIBER_HAS_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace fiber {

// Per-instance, not a single program-wide constant: different fibers
// legitimately need different worst-case stack depth (a shallow dispatch
// loop vs. one that calls into a deeply recursive parser), so hardcoding
// one size for every fiber in a program is exactly what the established
// stackful-fiber libraries avoid - see e.g. Boost.Context's
// fixedsize_stack/protected_fixedsize_stack, which both take the size as a
// constructor argument with a library-chosen default, never a fixed
// program-wide value. default_stack_size is that default here, not a cap.
constexpr size_t default_stack_size = 256 * 1024;
constexpr size_t guard_size = 4096; // one page
constexpr size_t min_stack_size = 4 * guard_size; // enough to not immediately overflow entering trampoline() itself

class scheduler;

class fiber {
public:
    explicit fiber(std::function<void()> fn, size_t stack_size = default_stack_size)
        : fn(std::move(fn)), stack_size(stack_size) {
        if (stack_size < min_stack_size) throw std::invalid_argument("fiber: stack_size too small");

        // A guard page below the usable stack (stacks grow down, so it
        // sits at the lowest address of the mapping) turns an overflow
        // into a clean SIGSEGV at the point it happens instead of silent
        // corruption of whatever heap allocation used to land next to a
        // plain malloc'd buffer - same principle as crypto::secret_buffer's
        // guard pages in the protocol project, applied here for the same
        // reason: catch it at the fault, not three allocations later.
        void* region = ::mmap(nullptr, stack_size + guard_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED) throw std::runtime_error("fiber: mmap failed");
        if (::mprotect(region, guard_size, PROT_NONE) != 0) {
            ::munmap(region, stack_size + guard_size);
            throw std::runtime_error("fiber: mprotect failed");
        }
        stack = region;
        stack_top = (uint8_t*)(region) + guard_size;

        getcontext(&ctx);
        ctx.uc_stack.ss_sp = stack_top;
        ctx.uc_stack.ss_size = stack_size;
        ctx.uc_link = &caller_ctx; // fn returning normally -> back to whoever resumed us

        // makecontext accepts only int arguments - pack the this pointer
        // into a pair of 32-bit halves (the standard trick on 64-bit
        // systems, since a bare pointer isn't a portable varargs type here).
        auto self = (uintptr_t)(this);
        makecontext(&ctx, (void(*)())(&fiber::trampoline), 2,
                    (unsigned)(self >> 32), (unsigned)(self & 0xffffffffu));
    }

    ~fiber() { ::munmap(stack, stack_size + guard_size); }
    fiber(const fiber&) = delete;
    fiber& operator=(const fiber&) = delete;

    bool done() const noexcept { return finished; }

    // Switches TO this fiber; returns here either via yield() or fn's
    // normal return/throw. Rethrows whatever fn threw, if anything - see
    // trampoline()'s comment on why the exception is captured there rather
    // than left to unwind across the raw ucontext switch.
    void resume() {
        auto* prev = current;
        current = this;
#if FIBER_HAS_ASAN
        void* fake_stack_save = nullptr;
        __sanitizer_start_switch_fiber(&fake_stack_save, stack_top, stack_size);
#endif
        swapcontext(&caller_ctx, &ctx);
#if FIBER_HAS_ASAN
        __sanitizer_finish_switch_fiber(fake_stack_save, nullptr, nullptr);
#endif
        current = prev;
        if (pending_exception) std::rethrow_exception(std::exchange(pending_exception, nullptr));
    }

private:
    static void trampoline(unsigned hi, unsigned lo) {
        uintptr_t self = ((uintptr_t)(hi) << 32) | lo;
        auto* f = (fiber*)(self);
#if FIBER_HAS_ASAN
        __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
        // Caught here, not left to unwind across swapcontext: a raw
        // ucontext switch is not exception-unwind-table-aware the way an
        // ordinary call stack is, so letting an exception propagate
        // directly out of fn() across it isn't something to rely on.
        // Stashing it and rethrowing from resume() (back on the caller's
        // own, ordinary stack) is the same pattern std::promise/std::future
        // use to move an exception across a "computed somewhere else"
        // boundary.
        try {
            f->fn();
        } catch (...) {
            f->pending_exception = std::current_exception();
        }
        f->finished = true;
#if FIBER_HAS_ASAN
        __sanitizer_start_switch_fiber(nullptr, nullptr, 0); // definite exit - never resumed again
#endif
        swapcontext(&f->ctx, &f->caller_ctx);
    }

    std::function<void()> fn;
    size_t stack_size;
    ucontext_t ctx{};
    ucontext_t caller_ctx{};
    void* stack = nullptr;
    void* stack_top = nullptr;
    bool finished = false;
    std::exception_ptr pending_exception;

    // thread_local, not a single global: see the file header - each OS
    // thread's scheduler must track its own "currently running fiber"
    // independently of every other thread's scheduler.
    static thread_local inline fiber* current;
    friend void yield();
};

// call from INSIDE a fiber - hands control back to whoever called resume()
inline void yield() {
    if (auto* f = fiber::current) {
#if FIBER_HAS_ASAN
        __sanitizer_start_switch_fiber(nullptr, nullptr, 0);
#endif
        swapcontext(&f->ctx, &f->caller_ctx);
#if FIBER_HAS_ASAN
        __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
    }
}

class scheduler {
public:
    fiber& spawn(std::function<void()> fn, size_t stack_size = default_stack_size) {
        fibers.push_back(std::make_unique<fiber>(std::move(fn), stack_size));
        return *fibers.back();
    }

    // One pass over every not-yet-finished fiber. A fiber whose fn()
    // threw has its exception captured here, not propagated immediately -
    // one fiber's failure must not stop every other still-live fiber from
    // getting its turn in the same pass. Returned in spawn-ish order for
    // the caller to log/rethrow/handle however it wants; call in a loop
    // until has_pending() is false.
    std::vector<std::exception_ptr> run_once() {
        std::vector<std::exception_ptr> failures;
        for (auto it = fibers.begin(); it != fibers.end();) {
            if ((*it)->done()) { it = fibers.erase(it); continue; }
            try {
                (*it)->resume();
            } catch (...) {
                failures.push_back(std::current_exception());
            }
            ++it;
        }
        return failures;
    }

    bool has_pending() const noexcept { return !fibers.empty(); }

private:
    std::vector<std::unique_ptr<fiber>> fibers;
};

} // namespace fiber
