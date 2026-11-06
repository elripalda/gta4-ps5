#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_CAPTURE_RING_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_CAPTURE_RING_H_

// [ps5 port] Capture on its own core: the byte ring between the game's render
// thread (and any other thread submitting title commands, one at a time under
// the submitters' lock) and the capture thread, which runs the capture the
// render thread used to run itself.
//
// Records are 16-byte aligned and never split across the end: a record that
// would not fit leaves a wrap mark (kWrapMark in its first word) and starts
// again at the front. Head and tail only grow; each side keeps a copy of the
// other's position and reads the real one only when that copy says the ring is
// full (or empty), so the two threads don't trade a cache line per record.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace rex::graphics::gta4_native {

class NativeCaptureRing {
 public:
  static constexpr size_t kCapacity = size_t(8) << 20;
  static constexpr uint32_t kWrapMark = 0xFFFFFFFFu;
  static constexpr size_t kAlign = 16;

  NativeCaptureRing() {
    data_ = static_cast<uint8_t*>(std::aligned_alloc(4096, kCapacity));
  }
  ~NativeCaptureRing() { std::free(data_); }
  NativeCaptureRing(const NativeCaptureRing&) = delete;
  NativeCaptureRing& operator=(const NativeCaptureRing&) = delete;

  bool valid() const { return data_ != nullptr; }
  static constexpr size_t Align(size_t size) { return (size + kAlign - 1) & ~(kAlign - 1); }

  // ---- Producer (one at a time) ----
  // Room for `size` bytes (aligned), waiting for the capture thread to make
  // some; the wait in microseconds is added to *waited_us.
  uint8_t* Reserve(size_t size, uint64_t* waited_us) {
    const size_t offset = size_t(head_ & (kCapacity - 1));
    skip_ = offset + size > kCapacity ? kCapacity - offset : 0;
    const uint64_t end = head_ + skip_ + size;
    if (end - cached_tail_ > kCapacity) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (end - cached_tail_ > kCapacity) {
        const auto begin = std::chrono::steady_clock::now();
        WaitFor([&] {
          cached_tail_ = tail_.load(std::memory_order_acquire);
          return end - cached_tail_ <= kCapacity;
        });
        if (waited_us)
          *waited_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count());
      }
    }
    if (skip_) {
      const uint32_t mark = kWrapMark;
      std::memcpy(data_ + offset, &mark, sizeof(mark));
    }
    return data_ + size_t((head_ + skip_) & (kCapacity - 1));
  }
  // Publishes the record Reserve returned. A plain store: a locked one would
  // wait for the capture thread's copy of the line every record. A present
  // (and every record while the capture thread sleeps) is fenced, so the
  // capture thread is never left asleep with a frame's end in the ring; a
  // record whose wake raced the capture thread falling asleep is found by
  // the next record, or within a millisecond (WaitForRecords).
  void Commit(size_t size, bool present) {
    head_ += skip_ + size;
    skip_ = 0;
    if (present) presents_.fetch_add(1, std::memory_order_relaxed);
    head_published_.store(head_, std::memory_order_release);
    if (present) std::atomic_thread_fence(std::memory_order_seq_cst);
    if (consumer_sleeping_.load(std::memory_order_relaxed)) {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      std::lock_guard lock(mutex_);
      consumer_condition_.notify_one();
    }
  }
  uint64_t head() const { return head_; }
  uint32_t presents_queued() const { return presents_.load(std::memory_order_acquire); }
  // Waits (spinning a little, then sleeping) until `done` holds; the capture
  // thread wakes producers after every record while one is waiting.
  template <typename Done>
  void WaitFor(Done&& done) {
    for (int spin = 0; spin < 2000; ++spin) {
      if (done() || stopped_.load(std::memory_order_relaxed)) return;
      Pause();
    }
    std::unique_lock lock(mutex_);
    producer_waiting_.fetch_add(1, std::memory_order_seq_cst);
    while (!done() && !stopped_.load(std::memory_order_acquire))
      producer_condition_.wait_for(lock, std::chrono::milliseconds(2));
    producer_waiting_.fetch_sub(1, std::memory_order_relaxed);
  }
  // At shutdown: nothing will be captured any more; every wait ends.
  void Stop() {
    stopped_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    producer_condition_.notify_all();
    consumer_condition_.notify_all();
  }
  bool stopped() const { return stopped_.load(std::memory_order_acquire); }
  // Every record published before this call (by any thread) has been processed.
  void WaitProcessed(uint64_t target) {
    if (tail_.load(std::memory_order_acquire) >= target) return;
    WakeConsumer();
    WaitFor([&] { return tail_.load(std::memory_order_acquire) >= target; });
  }

  // ---- Capture thread ----
  // The next record (nullptr when empty), past any wrap mark.
  const uint8_t* Peek() {
    for (;;) {
      if (consumer_tail_ == cached_head_) {
        cached_head_ = head_published_.load(std::memory_order_acquire);
        if (consumer_tail_ == cached_head_) return nullptr;
      }
      const size_t offset = size_t(consumer_tail_ & (kCapacity - 1));
      uint32_t first;
      std::memcpy(&first, data_ + offset, sizeof(first));
      if (first == kWrapMark) {
        consumer_tail_ += kCapacity - offset;
        continue;
      }
      return data_ + offset;
    }
  }
  // Done with the record Peek returned (`size` aligned bytes).
  // (Fenced as Commit is: a waiting producer also looks every 2 ms.)
  void Pop(size_t size, bool present) {
    consumer_tail_ += size;
    if (present) presents_.fetch_sub(1, std::memory_order_release);
    tail_.store(consumer_tail_, std::memory_order_release);
    if (present) std::atomic_thread_fence(std::memory_order_seq_cst);
    if (producer_waiting_.load(std::memory_order_relaxed)) {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      std::lock_guard lock(mutex_);
      producer_condition_.notify_all();
    }
  }
  // Waits for records (spinning about `spin_us` first); false once stopped.
  bool WaitForRecords(const std::atomic<bool>& running, uint32_t spin_us) {
    const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
    uint32_t round = 0;
    while (running.load(std::memory_order_relaxed)) {
      if (Ready()) return true;
      Pause();
      if ((++round & 63) == 0 && std::chrono::steady_clock::now() >= spin_until) break;
    }
    std::unique_lock lock(mutex_);
    consumer_sleeping_.store(true, std::memory_order_seq_cst);
    while (running.load(std::memory_order_relaxed) && !Ready())
      consumer_condition_.wait_for(lock, std::chrono::milliseconds(1));
    consumer_sleeping_.store(false, std::memory_order_relaxed);
    return running.load(std::memory_order_relaxed);
  }
  void WakeConsumer() {
    std::lock_guard lock(mutex_);
    consumer_condition_.notify_all();
  }

 private:
  bool Ready() {
    cached_head_ = head_published_.load(std::memory_order_acquire);
    return consumer_tail_ != cached_head_;
  }
  static void Pause() {
#if defined(__x86_64__)
    _mm_pause();
#endif
  }

  uint8_t* data_ = nullptr;
  // Producer side.
  alignas(64) uint64_t head_ = 0;
  uint64_t cached_tail_ = 0;
  size_t skip_ = 0;
  alignas(64) std::atomic<uint64_t> head_published_{0};
  // Capture thread side.
  alignas(64) uint64_t consumer_tail_ = 0;
  uint64_t cached_head_ = 0;
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) std::atomic<uint32_t> presents_{0};
  std::atomic<bool> consumer_sleeping_{false};
  std::atomic<uint32_t> producer_waiting_{0};
  std::atomic<bool> stopped_{false};
  std::mutex mutex_;
  std::condition_variable consumer_condition_, producer_condition_;
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_CAPTURE_RING_H_
