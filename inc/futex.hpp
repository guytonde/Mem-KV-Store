#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kvstore {
namespace util {

// Park/unpark on an address. On Linux these are the real futex syscalls but only
// FUTEX_WAIT and FUTEX_WAKE. The return value is dropped since every caller 
// rechecks its own predicate in a loop. These locks work within only 1 process 
// and will break in shared memory across processes. Elsewhere there is no wake 
// path so futex_wait is a yielding spin.

// Spin-wait hint to the CPU
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// sleep until *addr stops being expected and someone wakes
inline void futex_wait(std::atomic<uint32_t>* addr, uint32_t expected) noexcept {
#if defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT_PRIVATE,
              static_cast<int>(expected), nullptr, nullptr, 0);
#else
    // for non linux want to do yielding spin 
    if (addr->load(std::memory_order_relaxed) == expected) std::this_thread::yield();
#endif
}

// Wake up to count threads sleeping on *addr.
inline void futex_wake(std::atomic<uint32_t>* addr, int count) noexcept {
#if defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE_PRIVATE, count, nullptr,
              nullptr, 0);
#else
    (void)addr;
    (void)count;
#endif
}

inline void futex_wake_all(std::atomic<uint32_t>* addr) noexcept {
#if defined(__linux__)
    futex_wake(addr, INT_MAX);
#else
    (void)addr;
#endif
}

// Spins in userspace before we pay for a syscall
inline constexpr int kSpinCount = 64;

} // namespace util
} // namespace kvstore
