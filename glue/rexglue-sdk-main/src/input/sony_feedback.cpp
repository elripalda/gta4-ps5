#include <rex/input/sony_feedback.h>

#include <algorithm>
#include <cmath>

namespace rex::input::sony {
namespace {
constexpr uint64_t kLeaseMs = 250;
constexpr uint64_t kGestureExpiryMs = 500;
constexpr uint64_t kSwipeWindowMs = 700;
constexpr uint64_t kCinematicHoldMs = 300;
constexpr std::array<uint64_t, 3> kPulseMs{80, 150, 300};

float Unit(float value) {
  return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}
uint16_t Motor(float value) { return static_cast<uint16_t>(Unit(value) * 65535.0f); }

// GTA IV's guns by weapon slot. Ready to fire, the trigger has a wall that
// gives way (weapon mode: where it starts and ends, how hard); a single shot
// kicks it (vibration: amplitude, Hz, how long); held automatic fire knocks
// it once a round (its frequency the measured rate of fire). Each shot also
// shakes the controller (low and high motors, how long).
struct GunFeel {
  uint8_t start = 0, end = 0, strength = 0;  // the trigger before a shot
  uint8_t aim = 0;                           // L2 while aiming (feedback)
  uint8_t kick = 0, kick_hz = 0;             // a single shot's kick (0 Hz: 30)
  uint16_t kick_ms = 0;
  float low = 0, high = 0;                   // the body rumble of a shot
  uint16_t rumble_ms = 0;
};
constexpr std::array<GunFeel, 10> kGuns{{
    {},                                          // 0 unarmed
    {},                                          // 1 melee
    {3, 5, 5, 2, 8, 30, 70, 0.30f, 0.70f, 70},   // 2 handgun: light take-up, crisp break
    {3, 6, 8, 3, 8, 18, 130, 1.00f, 0.80f, 150}, // 3 shotgun: heavy, a deep thump
    {2, 4, 3, 2, 6, 0, 45, 0.18f, 0.50f, 45},    // 4 SMG: light, rattles while held
    {3, 5, 4, 3, 7, 0, 55, 0.28f, 0.60f, 55},    // 5 rifle
    {4, 6, 8, 4, 8, 22, 140, 0.90f, 0.95f, 170}, // 6 sniper: heavy, one big kick
    {4, 7, 8, 5, 8, 14, 220, 1.00f, 0.70f, 300}, // 7 heavy: rockets
    {2, 3, 2, 1, 0, 0, 0, 0.00f, 0.00f, 0},      // 8 thrown: a light pull
    {},                                          // 9 special
}};
const GunFeel& Gun(int slot) { return kGuns[size_t(std::clamp(slot, 0, 9))]; }

// The console's trigger modes as DualSense report bytes, for the drivers that
// send those (the PS5's reads TriggerFeel itself).
Trigger Report(const TriggerFeel& feel) {
  Trigger result{};
  switch (feel.kind) {
    case TriggerFeel::kFeedback:
    case TriggerFeel::kWeapon:
      return Resistance(uint8_t(std::min(feel.a * 28, 255)),
                        uint8_t(std::min((feel.kind == TriggerFeel::kWeapon ? feel.c : feel.b) * 13, 110)));
    case TriggerFeel::kVibration:
      if (!feel.b) break;
      result[0] = 0x06;
      result[1] = feel.c;
      result[2] = uint8_t(std::min(feel.b * 8, 63));
      result[3] = uint8_t(std::min(feel.a * 28, 255));
      return result;
    default: break;
  }
  return Resistance(0, 0);
}
}  // namespace

// These basic modes and byte layouts are exercised by the pinned SDL
// test/testcontroller.c CyclePS5TriggerEffect. SDL adds transport framing/CRC.
Trigger Resistance(uint8_t position, uint8_t strength) {
  Trigger result{};
  result[0] = strength ? 0x01 : 0x05;
  if (strength) {
    result[1] = position;
    result[2] = std::min<uint8_t>(strength, 110);
  }
  return result;
}

Trigger Recoil(uint8_t strength) {
  if (!strength) return Resistance(0, 0);
  Trigger result{};
  result[0] = 0x06;
  result[1] = 15;
  result[2] = std::min<uint8_t>(strength, 63);
  result[3] = 128;
  return result;
}

EffectPacket EncodeTriggers(const Trigger& left, const Trigger& right) {
  EffectPacket result{};
  result[0] = 0x0C;
  std::copy(right.begin(), right.end(), result.begin() + 10);
  std::copy(left.begin(), left.end(), result.begin() + 21);
  return result;
}

Service& GetService() {
  static Service service;
  return service;
}

bool Service::Fresh(const Slot& slot, uint64_t now_ms) {
  return slot.device.model != Model::kNone && slot.device.generation && slot.published &&
         now_ms >= slot.published_ms && now_ms - slot.published_ms <= kLeaseMs;
}

void Service::Cancel(Slot& slot) {
  CancelGestures(slot);
  slot.pulses = {};
  slot.shot_rumble_until_ms = 0;
}

void Service::CancelGestures(Slot& slot) {
  slot.pending.clear();
  for (auto& finger : slot.fingers) finger.cancelled = finger.down;
}

Service::Slot* Service::Find(uint32_t instance_id) {
  if (!instance_id) return nullptr;
  for (auto& slot : slots_) {
    if (slot.device.instance_id == instance_id) return &slot;
  }
  return nullptr;
}

void Service::Enqueue(Slot& slot, Gesture gesture, uint64_t now_ms) {
  if (slot.pending.size() < 16) slot.pending.push_back({gesture, now_ms});
}

void Service::Connect(uint32_t user, Device device) {
  std::lock_guard lock(mutex_);
  if (user >= slots_.size()) return;
  slots_[user] = {};
  if (device.model != Model::kNone && device.instance_id && device.generation) {
    device.triggers = device.triggers && device.model == Model::kDualSense;
    slots_[user].device = device;
  }
}

void Service::Disconnect(uint32_t instance_id) {
  std::lock_guard lock(mutex_);
  if (auto* slot = Find(instance_id)) *slot = {};
}

void Service::SetFocused(bool focused) {
  std::lock_guard lock(mutex_);
  focused_ = focused;
  if (!focused) {
    for (auto& slot : slots_) {
      Cancel(slot);
      slot.published = false;
    }
  }
}

void Service::UpdateAxes(uint32_t user, uint8_t left, uint8_t right) {
  std::lock_guard lock(mutex_);
  if (user >= slots_.size()) return;
  slots_[user].device.left_trigger = left;
  slots_[user].device.right_trigger = right;
}

void Service::DisableTriggers(uint32_t user, uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (user < slots_.size() && slots_[user].device.generation == generation)
    slots_[user].device.triggers = false;
}

Device Service::ReadDevice(uint32_t user) const {
  std::lock_guard lock(mutex_);
  return user < slots_.size() ? slots_[user].device : Device{};
}

void Service::ObserveTouch(uint32_t instance_id, int finger_index, ContactPhase phase,
                           float x, float y, uint64_t now_ms) {
  std::lock_guard lock(mutex_);
  auto* slot = Find(instance_id);
  if (!slot || !slot->device.touchpad || finger_index < 0 || finger_index >= 2) return;
  auto& finger = slot->fingers[static_cast<size_t>(finger_index)];
  const bool valid = std::isfinite(x) && std::isfinite(y) && x >= 0 && x <= 1 && y >= 0 && y <= 1;
  const bool owned = focused_ && Fresh(*slot, now_ms) && slot->game.active && slot->game.gestures_owned;
  if (phase == ContactPhase::kDown) {
    finger = {true, !owned || !valid, x, y, now_ms};
    if (slot->fingers[0].down && slot->fingers[1].down && !slot->multi_touch) {
      slot->multi_touch = true;
      slot->multi_started_ms = now_ms;
    }
  } else if (phase == ContactPhase::kUp) {
    if (finger.down && !finger.cancelled && !slot->multi_touch && owned && valid &&
        now_ms >= finger.started_ms && now_ms - finger.started_ms <= kSwipeWindowMs) {
      const float dx = x - finger.start_x, dy = y - finger.start_y;
      if (std::abs(dx) >= 0.20f && std::abs(dx) >= std::abs(dy) * 1.5f) {
        Enqueue(*slot, dx > 0 ? Gesture::kNextRadio : Gesture::kPreviousRadio, now_ms);
      } else if (std::abs(dy) >= 0.20f && std::abs(dy) >= std::abs(dx) * 1.5f) {
        Enqueue(*slot, dy > 0 ? Gesture::kNextWeapon : Gesture::kPreviousWeapon, now_ms);
      }
    }
    finger = {};
    if (!slot->fingers[0].down && !slot->fingers[1].down) slot->multi_touch = false;
  } else if (!owned || !valid) {
    finger.cancelled = true;
  }
}

void Service::ObserveClick(uint32_t instance_id, bool down, uint64_t now_ms) {
  std::lock_guard lock(mutex_);
  auto* slot = Find(instance_id);
  if (!slot || !slot->device.touchpad) return;
  if (down && !slot->click_down) {
    slot->click_owned = focused_ && Fresh(*slot, now_ms) && slot->game.active && slot->game.gestures_owned;
    if (slot->click_owned && !slot->multi_touch) Enqueue(*slot, Gesture::kCamera, now_ms);
  }
  slot->click_down = down;
  if (!down) slot->click_owned = false;
}

bool Service::ClaimsClick(uint32_t user, uint64_t now_ms, const Options& options) const {
  std::lock_guard lock(mutex_);
  if (user >= slots_.size()) return false;
  const auto& slot = slots_[user];
  if (slot.click_down) return slot.click_owned;
  return options.enabled && options.gestures && focused_ && Fresh(slot, now_ms) &&
         slot.game.active && slot.game.gestures_owned && slot.device.touchpad;
}

bool Service::OwnsGestures(uint32_t user, uint64_t now_ms, const Options& options) const {
  std::lock_guard lock(mutex_);
  if (user >= slots_.size()) return false;
  const auto& slot = slots_[user];
  return options.enabled && options.gestures && focused_ && Fresh(slot, now_ms) &&
         slot.game.active && slot.game.gestures_owned && slot.device.touchpad;
}

Input Service::Consume(uint32_t user, uint64_t now_ms, const Options& options) {
  std::lock_guard lock(mutex_);
  Input input{};
  if (user >= slots_.size()) return input;
  auto& slot = slots_[user];
  input.device = slot.device;
  if (!options.enabled || !focused_ || !Fresh(slot, now_ms) ||
      !slot.game.active || !slot.game.gestures_owned) {
    if (!options.enabled || !focused_ || !Fresh(slot, now_ms) || !slot.game.active)
      Cancel(slot);
    else
      CancelGestures(slot);
    return input;
  }
  if (!options.gestures) {
    CancelGestures(slot);
    return input;
  }
  input.gestures_owned = slot.device.touchpad;
  for (const auto& gesture : slot.pending) {
    if (now_ms >= gesture.timestamp && now_ms - gesture.timestamp <= kGestureExpiryMs)
      input.gestures.push_back(gesture.gesture);
  }
  slot.pending.clear();
  input.cinematic = slot.game.in_vehicle && slot.multi_touch &&
                    slot.fingers[0].down && slot.fingers[1].down &&
                    !slot.fingers[0].cancelled && !slot.fingers[1].cancelled &&
                    now_ms >= slot.multi_started_ms && now_ms - slot.multi_started_ms >= kCinematicHoldMs;
  return input;
}

void Service::Publish(uint32_t user, uint64_t generation, const GameState& state, uint64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (user >= slots_.size() || !generation || slots_[user].device.generation != generation) return;
  auto& slot = slots_[user];
  const bool identity_changed = slot.game.player_identity != state.player_identity ||
                                slot.game.context_identity != state.context_identity;
  if (!focused_ || !state.active || identity_changed ||
      (slot.game.gestures_owned && !state.gestures_owned) || !Fresh(slot, now_ms)) Cancel(slot);
  if (identity_changed || (slot.game.wanted <= 0 && state.wanted > 0)) slot.wanted_started_ms = now_ms;
  slot.game = state;
  slot.published_ms = now_ms;
  slot.published = focused_;
}

void Service::Emit(uint32_t user, uint64_t generation, Event event, float strength, uint64_t now_ms) {
  std::lock_guard lock(mutex_);
  const auto index = static_cast<size_t>(event);
  if (user >= slots_.size() || index >= kPulseMs.size() || !generation) return;
  auto& slot = slots_[user];
  if (!focused_ || slot.device.generation != generation || !Fresh(slot, now_ms) || !slot.game.active) return;
  if (event == Event::kShot) {
    // How fast the gun fires (shots under 0.4 s apart, same weapon slot), and
    // the controller's shake for this shot by the kind of gun. In rapid fire
    // each shake ends before the next shot, so the rounds are felt one by one.
    const int weapon = std::clamp(slot.game.weapon_slot, 0, 9);
    auto& shots = slot.shots;
    if (shots.last_ms && now_ms > shots.last_ms && now_ms - shots.last_ms < 400 && shots.slot == weapon) {
      const float gap = float(now_ms - shots.last_ms);
      shots.interval_ms = shots.interval_ms > 0 ? shots.interval_ms * 0.6f + gap * 0.4f : gap;
    } else {
      shots.interval_ms = 0;
    }
    shots.last_ms = now_ms;
    shots.slot = weapon;
    const GunFeel& gun = Gun(weapon);
    float ms = gun.rumble_ms ? gun.rumble_ms : 80.0f;
    if (shots.interval_ms > 0) ms = std::min(ms, std::max(25.0f, shots.interval_ms * 0.55f));
    slot.shot_rumble_until_ms = now_ms + uint64_t(ms);
    slot.shot_low = (gun.rumble_ms ? gun.low : 0.35f) * Unit(strength);
    slot.shot_high = (gun.rumble_ms ? gun.high : 1.0f) * Unit(strength);
    return;
  }
  auto& pulse = slot.pulses[index];
  const float previous = now_ms < pulse.until_ms ? pulse.strength : 0.0f;
  pulse = {now_ms + kPulseMs[index], std::max(previous, Unit(strength))};
}

Output Service::Compose(uint32_t user, uint64_t now_ms, const Options& options) const {
  std::lock_guard lock(mutex_);
  Output output{};
  output.left = output.right = Resistance(0, 0);
  if (user >= slots_.size()) return output;
  const auto& slot = slots_[user];
  output.suppress_native = !focused_ || (slot.published && (!Fresh(slot, now_ms) || !slot.game.active));
  if (!options.enabled || !focused_ || !Fresh(slot, now_ms) || !slot.game.active) return output;
  output.active = true;
  const float intensity = Unit(options.intensity);
  output.lighting = options.lighting && slot.device.light;
  if (slot.game.wanted > 0 && now_ms >= slot.wanted_started_ms) {
    output.color = ((now_ms - slot.wanted_started_ms) / 250) % 2 == 0
                       ? std::array<uint8_t, 3>{255, 0, 0} : std::array<uint8_t, 3>{0, 0, 255};
  } else {
    output.color = slot.game.episode == 1 ? std::array<uint8_t, 3>{255, 128, 0}
                   : slot.game.episode == 2 ? std::array<uint8_t, 3>{128, 0, 255}
                                           : std::array<uint8_t, 3>{0, 0, 255};
  }
  auto pulse = [&](Event event) {
    const auto& value = slot.pulses[static_cast<size_t>(event)];
    return now_ms < value.until_ms ? value.strength : 0.0f;
  };
  const float damage = pulse(Event::kDamage), explosion = pulse(Event::kExplosion);
  const bool shooting = now_ms < slot.shot_rumble_until_ms;
  const bool driving = slot.game.trigger_context == TriggerContext::kVehicle && slot.game.driver;
  const float speed = std::isfinite(slot.game.speed) ? std::max(slot.game.speed, 0.0f) : 0.0f;
  if (options.rumble && slot.device.rumble) {
    const float low = std::max({shooting ? slot.shot_low : 0.0f, damage * 0.75f, explosion});
    const float high = std::max({shooting ? slot.shot_high : 0.0f, damage * 0.5f, explosion * 0.65f});
    output.low_motor = Motor(low * intensity);
    output.high_motor = Motor(high * intensity);
  }
  if (options.triggers && slot.device.triggers && intensity > 0) {
    using Feel = TriggerFeel;
    // A strength on the console's 1-8 scale, by the intensity setting (as
    // given at the default 50%, half at 0%, one and a half times at 100%).
    auto scaled = [&](int value) {
      return uint8_t(std::clamp(int(std::lround(float(value) * (0.5f + intensity))), 1, 8));
    };
    auto set = [&](Feel& feel, Trigger& report, Feel::Kind kind, uint8_t a, uint8_t b, uint8_t c) {
      feel = {kind, intensity, a, b, c};
      report = Report(feel);
    };
    if (driving) {
      // Pedals, nothing shaking (1.0.3 Test 3's road hum and trigger buzz were
      // too much): the gas pushes back more the further it goes, and harder
      // the faster the car is going; the brake is a firm pedal throughout.
      const float throttle = slot.device.right_trigger / 255.0f, brake = slot.device.left_trigger / 255.0f;
      const float load = std::min(speed / 40.0f, 1.0f);
      set(output.right_feel, output.right, Feel::kFeedback, 1,
          scaled(1 + int(std::lround(3.0f * std::pow(throttle, 1.3f) + load))), 0);
      set(output.left_feel, output.left, Feel::kFeedback, 2,
          scaled(3 + int(std::lround(3.0f * brake))), 0);
    } else if (slot.game.trigger_context == TriggerContext::kWeapon) {
      const GunFeel& gun = Gun(slot.game.weapon_slot);
      if (slot.game.aiming && gun.aim) set(output.left_feel, output.left, Feel::kFeedback, 2, scaled(gun.aim), 0);
      if (slot.game.can_fire) {
        const auto& shots = slot.shots;
        const uint64_t since = now_ms >= shots.last_ms ? now_ms - shots.last_ms : UINT64_MAX;
        const bool rapid = shots.interval_ms > 0 && shots.interval_ms < 260 &&
                           float(since) < shots.interval_ms * 1.6f + 40.0f;
        if (rapid) {
          // held automatic fire: a knock a round
          set(output.right_feel, output.right, Feel::kVibration, 1, scaled(gun.kick ? gun.kick - 1 : 6),
              uint8_t(std::clamp(int(std::lround(1000.0f / shots.interval_ms)), 5, 30)));
        } else if (since < gun.kick_ms) {
          set(output.right_feel, output.right, Feel::kVibration, 1, scaled(gun.kick),
              gun.kick_hz ? gun.kick_hz : 30);
        } else if (gun.strength) {
          set(output.right_feel, output.right, Feel::kWeapon, gun.start, gun.end, scaled(gun.strength));
        }
      }
    }
  }
  return output;
}

}  // namespace rex::input::sony
