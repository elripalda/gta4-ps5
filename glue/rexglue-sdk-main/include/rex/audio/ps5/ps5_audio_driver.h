#pragma once
/**
 * [ps5 port] Audio out through sceAudioOut: the Xbox 360's 48 kHz frames of
 * 256 samples x 6 channels (FL, FR, FC, LFE, BL, BR), output as 8-channel
 * 16-bit surround (S16_8CH: FL, FR, FC, LFE, LS, RS, LRS, RRS with 5.1 rear
 * channels mapped to both surround pairs) with stereo 16-bit fallback,
 * pushed by a thread of its own.
 */
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::ps5 {

class Ps5AudioDriver : public AudioDriver {
 public:
  Ps5AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~Ps5AudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  // How far ahead of the speakers the game may be (ps5_audio_frames): each
  // frame queued is 5.3 ms later that a sound is heard.
  uint32_t RecommendedInitialCredits(uint32_t configured_maximum) const override;
  void Pause() override { paused_.store(true, std::memory_order_relaxed); }
  void Resume() override { paused_.store(false, std::memory_order_relaxed); }
  void Shutdown();

 private:
  void OutputThread();

  static constexpr uint32_t kFrequency = 48000;
  static constexpr uint32_t kChannels = 6;
  static constexpr uint32_t kStereoOutputChannels = 2;
  static constexpr uint32_t kSurroundOutputChannels = 8;
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameSamples = kChannels * kChannelSamples;

  rex::thread::Semaphore* semaphore_ = nullptr;
  int32_t port_ = -1;
  uint32_t output_channels_ = kSurroundOutputChannels;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> paused_{false};

  std::queue<float*> frames_queued_;
  std::stack<float*> frames_unused_;
  std::mutex frames_mutex_;
};

}  // namespace rex::audio::ps5
