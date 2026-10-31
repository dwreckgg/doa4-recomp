// src/net/np_spsc.h - single-producer single-consumer ring of T (header-only, pure; no SDK, no Windows).
// Holds up to N items (N a power of two). One thread pushes, one other thread pops; neither ever blocks or makes a
// system call. The producer stores the item into its slot BEFORE it publishes the new write index (a release store),
// and the consumer reads the write index with an acquire load before it reads the slot, so a slot is never read
// before it was written; the consumer frees the slot the same way (move out, then publish the read index).
// Host-tested by tests/host/np_transport_host_test.cpp (one thread, and two threads moving 1,000,000 items).
#pragma once
#include <atomic>
#include <cstddef>
#include <utility>

namespace doa4::np {

template <class T, size_t N>
class Spsc {  // N a power of two
  static_assert(N >= 2 && (N & (N - 1)) == 0, "Spsc: N must be a power of two");

 public:
  static constexpr size_t kCapacity = N;

  // Producer thread only. False (and `item` left as it was) when N items are queued.
  bool Push(T&& item) {
    const size_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) == N) return false;
    slots_[w & (N - 1)] = std::move(item);
    write_.store(w + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. False (and *item left as it was) when nothing is queued.
  bool Pop(T* item) {
    const size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    *item = std::move(slots_[r & (N - 1)]);
    read_.store(r + 1, std::memory_order_release);
    return true;
  }

 private:
  T slots_[N]{};
  alignas(64) std::atomic<size_t> write_{0};  // items ever pushed (written by the producer only)
  alignas(64) std::atomic<size_t> read_{0};   // items ever popped (written by the consumer only)
};

}  // namespace doa4::np
