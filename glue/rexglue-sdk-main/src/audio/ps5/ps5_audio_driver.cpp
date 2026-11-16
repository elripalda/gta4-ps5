/**
 * [ps5 port] sceAudioOut audio driver (see ps5_audio_driver.h).
 */
#include <rex/audio/ps5/ps5_audio_driver.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/logging.h>

extern "C" {
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                        uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void* ptr);
int32_t sceAudioOutClose(int32_t handle);
}

REXCVAR_DECLARE(bool, audio_mute);
// [ps5 port] The sound queued ahead of the speakers: 12 frames is 64 ms. The
// generic 64 (341 ms) put every sound a third of a second behind the picture
// and the controller.
REXCVAR_DEFINE_UINT32(ps5_audio_frames, 12, "PS5",
                      "Sound frames queued ahead of the speakers (5.3 ms each): fewer is less delay")
    .range(2, 64);

namespace rex::audio::ps5 {

namespace {
constexpr int32_t kSystemUser = 0xFF;
constexpr int32_t kPortTypeMain = 0;
constexpr uint32_t kFormatS16Stereo = 1;
constexpr uint32_t kFormatS16_8CH = 2;
}  // namespace

Ps5AudioDriver::Ps5AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

Ps5AudioDriver::~Ps5AudioDriver() { Shutdown(); }

bool Ps5AudioDriver::Initialize() {
  int32_t rc = sceAudioOutInit();
  // (Already initialized by someone else in the process is fine.)
  (void)rc;
  output_channels_ = kSurroundOutputChannels;
  port_ = sceAudioOutOpen(kSystemUser, kPortTypeMain, 0, kChannelSamples, kFrequency,
                          kFormatS16_8CH);
  if (port_ < 0) {
    REXAPU_WARN("PS5 audio: 8ch sceAudioOutOpen failed ({:#x}); falling back to stereo",
                uint32_t(port_));
    output_channels_ = kStereoOutputChannels;
    port_ = sceAudioOutOpen(kSystemUser, kPortTypeMain, 0, kChannelSamples, kFrequency,
                            kFormatS16Stereo);
  }
  if (port_ < 0) {
    REXAPU_ERROR("PS5 audio: sceAudioOutOpen failed ({:#x})", uint32_t(port_));
    return false;
  }
  REXAPU_INFO("PS5 audio: port {} (48000 Hz, 256-sample frames, s16 {}ch)", port_,
              output_channels_);
  running_ = true;
  thread_ = std::thread([this] { OutputThread(); });
  return true;
}

uint32_t Ps5AudioDriver::RecommendedInitialCredits(uint32_t configured_maximum) const {
  const uint32_t frames = std::clamp<uint32_t>(REXCVAR_GET(ps5_audio_frames), 2, std::max(2u, configured_maximum));
  REXAPU_INFO("PS5 audio: {} frames ahead of the speakers ({:.0f} ms)", frames,
              double(frames) * kChannelSamples * 1000.0 / kFrequency);
  return frames;
}

void Ps5AudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      frame = new float[kFrameSamples];
    } else {
      frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }
  std::memcpy(frame, input_frame, kFrameSamples * sizeof(float));
  std::lock_guard<std::mutex> guard(frames_mutex_);
  frames_queued_.push(frame);
}

// sceAudioOutOutput blocks until the previous buffer has started playing, so
// this loop runs at exactly the output rate (one frame per 5.33 ms): a queued
// game frame when there is one (then the game may submit another: release
// the semaphore), else silence.
void Ps5AudioDriver::OutputThread() {
  float samples[kChannelSamples * kChannels];
  int16_t pcm[kChannelSamples * kSurroundOutputChannels];
  const size_t pcm_bytes = kChannelSamples * output_channels_ * sizeof(int16_t);
  // Gaps: the game's sound not there in time (silence played instead), while
  // it isn't paused. Logged every 10 s there were any (a shorter queue,
  // ps5_audio_frames, leaves less room for the game's threads to be late).
  bool started = false, in_gap = false;
  uint32_t gaps = 0, silent = 0, played = 0;
  while (running_) {
    float* frame = nullptr;
    {
      std::lock_guard<std::mutex> guard(frames_mutex_);
      if (!frames_queued_.empty()) {
        frame = frames_queued_.front();
        frames_queued_.pop();
      }
    }
    if (frame) {
      started = true, in_gap = false;
    } else if (started && !paused_.load(std::memory_order_relaxed)) {
      gaps += in_gap ? 0 : 1;
      in_gap = true;
      ++silent;
    }
    if (++played == kFrequency * 10 / kChannelSamples) {
      if (gaps) {
        REXAPU_INFO("PS5 audio: {} gaps in 10 s ({:.0f} ms of silence); {} frames ahead", gaps,
                    double(silent) * kChannelSamples * 1000.0 / kFrequency, REXCVAR_GET(ps5_audio_frames));
      }
      gaps = silent = played = 0;
    }
    if (frame && !REXCVAR_GET(audio_mute)) {
      if (output_channels_ == kSurroundOutputChannels) {
        // Xbox 360 XAudio2 canonical 5.1 order is (FL, FR, FC, LFE, BL, BR).
        // PS5 S16_8CH has 8 channels (FL, FR, FC, LFE, LS/BL, RS/BR, LRS/SL, RRS/SR);
        // duplicate the 5.1 rear channels (4, 5) into channels (6, 7) so both
        // side-surround and rear-back beds in 5.1 / 7.1 / Dolby Atmos receive them.
        conversion::sequential_6_BE_to_interleaved_6_LE(samples, frame, kChannelSamples);
        for (uint32_t s = 0; s < kChannelSamples; ++s) {
          const float* in = &samples[s * kChannels];
          int16_t* out = &pcm[s * kSurroundOutputChannels];
          for (uint32_t c = 0; c < kChannels; ++c) {
            out[c] = int16_t(std::clamp(in[c], -1.0f, 1.0f) * 32767.0f);
          }
          out[6] = out[4];
          out[7] = out[5];
        }
      } else {
        conversion::sequential_6_BE_to_interleaved_2_LE(samples, frame, kChannelSamples);
        for (uint32_t i = 0; i < kChannelSamples * kStereoOutputChannels; ++i) {
          float s = std::clamp(samples[i], -1.0f, 1.0f);
          pcm[i] = int16_t(s * 32767.0f);
        }
      }
    } else {
      std::memset(pcm, 0, pcm_bytes);
    }
    if (frame) {
      {
        std::lock_guard<std::mutex> guard(frames_mutex_);
        frames_unused_.push(frame);
      }
      semaphore_->Release(1, nullptr);
    }
    sceAudioOutOutput(port_, pcm);
  }
}

void Ps5AudioDriver::Shutdown() {
  if (running_.exchange(false) && thread_.joinable()) {
    thread_.join();
  }
  if (port_ >= 0) {
    sceAudioOutOutput(port_, nullptr);  // (waits for the last buffer)
    sceAudioOutClose(port_);
    port_ = -1;
  }
  std::lock_guard<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

}  // namespace rex::audio::ps5
