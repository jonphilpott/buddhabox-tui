/**
 * @file ring.h
 * @brief A lock-free queue for one writer thread and one reader thread.
 *
 * The audio input, the MIDI input and the audio thread each hand data to
 * another thread. A mutex would work, but the audio thread must never
 * wait for one: if the other thread holds it when the sound card wants
 * samples, the sound glitches. This queue needs no lock.
 *
 * HOW IT WORKS: two counters that only ever go up. The writer owns
 * `write_`, the reader owns `read_`; each only reads the other's. The
 * slot for count n is n % N. The queue holds write_ - read_ items (the
 * subtraction is right even after the counters wrap past 2^32, because
 * unsigned arithmetic wraps the same way).
 *
 * The std::atomic loads and stores with acquire/release ordering are
 * what make it safe: a release store says "everything I wrote before
 * this is visible to whoever acquires it", so the reader never sees the
 * counter move before the data it guards.
 *
 * @tparam T  The item type (copied in and out)
 * @tparam N  Capacity; a power of two keeps n % N cheap
 *
 * Example:
 *   Ring<int, 64> queue;
 *   queue.push(42);           // writer thread
 *   int v; if (queue.pop(v))  // reader thread
 *     use(v);
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace bb {

template <typename T, uint32_t N> class Ring {
public:
  /**
   * Add an item (writer thread only).
   * @param v  The item
   * @return   false if the queue was full (the item is dropped)
   */
  bool push(const T &v) {
    uint32_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) >= N)
      return false;
    buf_[w % N] = v;
    write_.store(w + 1, std::memory_order_release);
    return true;
  }

  /**
   * Take the oldest item (reader thread only).
   * @param v  Filled with the item
   * @return   false if the queue was empty
   */
  bool pop(T &v) {
    uint32_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire))
      return false;
    v = buf_[r % N];
    read_.store(r + 1, std::memory_order_release);
    return true;
  }

  /** @return  How many items wait (reader thread only). */
  uint32_t size() const {
    return write_.load(std::memory_order_acquire) -
           read_.load(std::memory_order_relaxed);
  }

private:
  T buf_[N];
  std::atomic<uint32_t> write_{0}, read_{0};
};

} // namespace bb
