/**
 * [ps5 port] DualSense input via scePad (layout verified on hardware by
 * Nazky's ps5-plume-triangle).
 *
 * Mapping (Xbox 360 <- DualSense): A <- Cross, B <- Circle, X <- Square,
 * Y <- Triangle, LB/RB <- L1/R1, LT/RT <- L2/R2 (analog), START <- Options,
 * BACK <- touchpad click, stick clicks <- L3/R3, d-pad <- d-pad.
 *
 * And the DualSense features the PC port has (rex::input::sony, Settings'
 * DualSense Features): the light bar (the game's colour, police lights while
 * wanted), adaptive triggers (a gun's trigger pull and its recoil, aiming,
 * pedals) and added rumble for shots, hits and explosions. Only the
 * controller's output: the game's own rumble is kept as it was. Rumble needs
 * the controller's compatible dual-motor mode (scePadSetVibrationMode 2, as
 * Porpoise and ProsperoLight set it): without it the PS5 drops it.
 */
#include <rex/input/ps5/ps5_input_driver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <SDL3/SDL_timer.h>

#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int32_t* user_id);
int sceUserServiceGetInitialUser(int32_t* user_id);
struct SceUserServiceLoginUserIdList {
  int32_t user_id[4];
};
int sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList* list);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
int scePadClose(int32_t handle);
struct ScePadVibrationParam {
  uint8_t large_motor;
  uint8_t small_motor;
};
int scePadSetVibration(int32_t handle, const ScePadVibrationParam* param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
struct ScePadLightBarParam {
  uint8_t r, g, b, reserved;
};
int scePadSetLightBar(int32_t handle, const ScePadLightBarParam* param);
int scePadResetLightBar(int32_t handle);
// The adaptive triggers: one command per trigger, its mode's parameters in
// data (the layout Porpoise uses on the console, after ProsperoEden's
// ps5_pad.hpp). Modes: 0 off, 1 feedback (data: position 0-9, strength 1-8),
// 2 weapon (start 2-7, end start+1..8, strength 0-8), 3 vibration (position
// 0-9, amplitude 0-8, frequency 0-255 Hz).
struct ScePadTriggerEffectCommand {
  int32_t mode;
  int32_t reserved;
  uint8_t data[48];
};
struct ScePadTriggerEffectParam {
  uint8_t trigger_mask;  // 1 L2, 2 R2
  uint8_t reserved[7];
  ScePadTriggerEffectCommand l2, r2;
};
static_assert(sizeof(ScePadTriggerEffectCommand) == 56 && sizeof(ScePadTriggerEffectParam) == 120,
              "the console's trigger effect layout");
int scePadSetTriggerEffect(int32_t handle, const ScePadTriggerEffectParam* param);
}

namespace rex::input::ps5 {

namespace {

constexpr int32_t kInvalidUser = -1;
constexpr int32_t kSystemUser = 0xFF;

// The head of ScePadData (the rest - touch, motion, ... - is unused).
struct ScePadDataHead {
  uint32_t buttons;
  uint8_t left_stick_x;
  uint8_t left_stick_y;
  uint8_t right_stick_x;
  uint8_t right_stick_y;
  uint8_t analog_l2;
  uint8_t analog_r2;
  uint16_t padding;
  uint8_t rest[240];  // room for the whole ScePadData
};

constexpr uint32_t kPadL3 = 0x00000002u;
constexpr uint32_t kPadR3 = 0x00000004u;
constexpr uint32_t kPadOptions = 0x00000008u;
constexpr uint32_t kPadUp = 0x00000010u;
constexpr uint32_t kPadRight = 0x00000020u;
constexpr uint32_t kPadDown = 0x00000040u;
constexpr uint32_t kPadLeft = 0x00000080u;
constexpr uint32_t kPadL1 = 0x00000400u;
constexpr uint32_t kPadR1 = 0x00000800u;
constexpr uint32_t kPadTriangle = 0x00001000u;
constexpr uint32_t kPadCircle = 0x00002000u;
constexpr uint32_t kPadCross = 0x00004000u;
constexpr uint32_t kPadSquare = 0x00008000u;
constexpr uint32_t kPadTouchpad = 0x00100000u;

// 0..255 (128 centre) to the Xbox's -32768..32767, Y flipped so up is +.
int16_t AxisToXbox(uint8_t value, bool flip) {
  int v = (int(value) - 128) * 256;
  if (flip) v = -v - 1;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return int16_t(v);
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// A strength (0..1) on the console's scale lo..hi.
uint8_t Level(float strength, int lo, int hi) {
  const float s = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
  return uint8_t(std::clamp(int(std::lround(float(lo) + float(hi - lo) * s)), lo, hi));
}

// A trigger's feel as the console's trigger modes: a gun's trigger gives way
// partway, as a real one breaks (weapon mode), and each shot kicks it
// (vibration); aiming and pedals push back evenly (feedback).
void FillTrigger(ScePadTriggerEffectCommand& c, const rex::input::sony::TriggerFeel& feel) {
  using Feel = rex::input::sony::TriggerFeel;
  std::memset(&c, 0, sizeof(c));
  switch (feel.kind) {
    case Feel::kPedal:
      c.mode = 1, c.data[0] = 1, c.data[1] = Level(feel.strength, 1, 8);
      break;
    case Feel::kAim:
      c.mode = 1, c.data[0] = 2, c.data[1] = Level(feel.strength, 1, 4);
      break;
    case Feel::kGun:
      c.mode = 2, c.data[0] = 4, c.data[1] = 6, c.data[2] = Level(feel.strength, 3, 8);
      break;
    case Feel::kRecoil:
      c.mode = 3, c.data[0] = 2, c.data[1] = Level(feel.strength, 2, 8), c.data[2] = 40;
      break;
    // (the mode and its parameters as given: Service::Compose's guns and driving)
    case Feel::kFeedback:
      c.mode = 1, c.data[0] = std::min<uint8_t>(feel.a, 9), c.data[1] = std::clamp<uint8_t>(feel.b, 1, 8);
      break;
    case Feel::kWeapon: {
      const uint8_t start = std::clamp<uint8_t>(feel.a, 2, 7);
      c.mode = 2, c.data[0] = start, c.data[1] = std::clamp<uint8_t>(feel.b, uint8_t(start + 1), 8),
      c.data[2] = std::min<uint8_t>(feel.c, 8);
      break;
    }
    case Feel::kVibration:
      if (!feel.b) break;
      c.mode = 3, c.data[0] = std::min<uint8_t>(feel.a, 9), c.data[1] = std::min<uint8_t>(feel.b, 8),
      c.data[2] = feel.c;
      break;
    default: break;  // (mode 0: off)
  }
}

}  // namespace

Ps5InputDriver::Ps5InputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

Ps5InputDriver::~Ps5InputDriver() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (uint32_t i = 0; i < kMaxPads; ++i) {
    Pad& pad = pads_[i];
    if (pad.handle < 0) continue;
    // (the controller as the system had it: its light, no trigger effects, still)
    if (pad.light_set) scePadResetLightBar(pad.handle);
    if (pad.triggers_set) {
      ScePadTriggerEffectParam off{};
      off.trigger_mask = 1 | 2;
      scePadSetTriggerEffect(pad.handle, &off);
    }
    ScePadVibrationParam still{0, 0};
    scePadSetVibration(pad.handle, &still);
    rex::input::sony::GetService().Disconnect(uint32_t(pad.handle) + 1);
    scePadClose(pad.handle);
  }
}

X_STATUS Ps5InputDriver::Setup() {
  sceUserServiceInitialize(nullptr);
  int rc = scePadInit();
  if (rc < 0) {
    REXLOG_ERROR("PS5 input: scePadInit failed ({:#x})", uint32_t(rc));
    return X_STATUS_UNSUCCESSFUL;
  }
  rex::input::sony::GetService().SetFocused(true);
  std::lock_guard<std::mutex> lock(mutex_);
  RefreshPads();
  return X_STATUS_SUCCESS;
}

// Opens a pad for every logged-in user that hasn't one yet (users log in and
// controllers connect while the game runs). Called at most twice a second.
void Ps5InputDriver::RefreshPads() {
  SceUserServiceLoginUserIdList users;
  for (int32_t& u : users.user_id) u = kInvalidUser;
  if (sceUserServiceGetLoginUserIdList(&users) < 0) {
    int32_t fg = kInvalidUser;
    if (sceUserServiceGetForegroundUser(&fg) == 0) users.user_id[0] = fg;
  }
  for (int32_t user : users.user_id) {
    if (user == kInvalidUser || user == kSystemUser) continue;
    bool known = false;
    for (const Pad& pad : pads_) known |= pad.user_id == user && pad.handle >= 0;
    if (known) continue;
    for (uint32_t i = 0; i < kMaxPads; ++i) {
      Pad& pad = pads_[i];
      if (pad.handle >= 0) continue;
      int32_t handle = scePadOpen(user, 0, 0, nullptr);
      if (handle < 0) handle = scePadGetHandle(user, 0, 0);
      if (handle < 0) break;
      pad.user_id = user;
      pad.packet_number = 0;
      pad.generation = next_generation_++;
      // the DualSense features: this controller has them all
      rex::input::sony::Device device{};
      device.instance_id = uint32_t(handle) + 1;
      device.generation = pad.generation;
      device.model = rex::input::sony::Model::kDualSense;
      device.light = device.rumble = device.triggers = true;
      rex::input::sony::GetService().Connect(i, device);
      pad.handle = handle;
      const int mode = scePadSetVibrationMode(handle, 2);  // (rumble: see the top)
      REXLOG_INFO("PS5 input: DualSense for user {:#x} (handle {}, rumble mode {:#x})", uint32_t(user), handle,
                  uint32_t(mode));
      break;
    }
  }
  last_refresh_ms_ = NowMs();
}

Ps5InputDriver::Pad* Ps5InputDriver::PadFor(uint32_t user_index) {
  if (user_index >= kMaxPads) return nullptr;
  if (NowMs() - last_refresh_ms_ > 500) RefreshPads();
  Pad& pad = pads_[user_index];
  return pad.handle >= 0 ? &pad : nullptr;
}

X_RESULT Ps5InputDriver::ReadState(uint32_t user_index, bool active, X_INPUT_STATE* out_state) {
  Pad* pad = PadFor(user_index);
  if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
  ScePadDataHead data;
  std::memset(&data, 0, sizeof(data));
  if (scePadReadState(pad->handle, &data) != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // (the pedals' push-back follows how far down they are)
  rex::input::sony::GetService().UpdateAxes(user_index, data.analog_l2, data.analog_r2);
  ApplyFeedback(user_index, *pad);
  if (!out_state) return X_ERROR_SUCCESS;
  const uint32_t b = active ? data.buttons : 0;
  uint16_t buttons = 0;
  if (b & kPadUp) buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (b & kPadDown) buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (b & kPadLeft) buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (b & kPadRight) buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (b & kPadOptions) buttons |= X_INPUT_GAMEPAD_START;
  if (b & kPadTouchpad) buttons |= X_INPUT_GAMEPAD_BACK;
  if (b & kPadL3) buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (b & kPadR3) buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (b & kPadL1) buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (b & kPadR1) buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (b & kPadCross) buttons |= X_INPUT_GAMEPAD_A;
  if (b & kPadCircle) buttons |= X_INPUT_GAMEPAD_B;
  if (b & kPadSquare) buttons |= X_INPUT_GAMEPAD_X;
  if (b & kPadTriangle) buttons |= X_INPUT_GAMEPAD_Y;

  X_INPUT_GAMEPAD gp{};
  gp.buttons = buttons;
  gp.left_trigger = active ? data.analog_l2 : 0;
  gp.right_trigger = active ? data.analog_r2 : 0;
  gp.thumb_lx = active ? AxisToXbox(data.left_stick_x, false) : 0;
  gp.thumb_ly = active ? AxisToXbox(data.left_stick_y, true) : 0;
  gp.thumb_rx = active ? AxisToXbox(data.right_stick_x, false) : 0;
  gp.thumb_ry = active ? AxisToXbox(data.right_stick_y, true) : 0;
  // The packet number only moves when the state does, as XInput's does.
  if (std::memcmp(&gp, &pad->last_gamepad, sizeof(gp)) != 0) {
    ++pad->packet_number;
    std::memcpy(&pad->last_gamepad, &gp, sizeof(gp));
  }
  out_state->gamepad = gp;
  out_state->packet_number = pad->packet_number;
  return X_ERROR_SUCCESS;
}

X_RESULT Ps5InputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadState(user_index, is_active(), out_state);
}

// The host's own menus (settings, dialogs) read the pad even while the game's
// input is held back.
X_RESULT Ps5InputDriver::GetStateUi(uint32_t user_index, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadState(user_index, true, out_state);
}

X_RESULT Ps5InputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!PadFor(user_index)) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xF3FF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
    out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT Ps5InputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  std::lock_guard<std::mutex> lock(mutex_);
  Pad* pad = PadFor(user_index);
  if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (vibration) {
    pad->native_low = vibration->left_motor_speed;
    pad->native_high = vibration->right_motor_speed;
    SendVibration(*pad);
  }
  return X_ERROR_SUCCESS;
}

// The game's rumble, with the added pulses over it (the stronger of each motor).
void Ps5InputDriver::SendVibration(Pad& pad) {
  ScePadVibrationParam param;
  param.large_motor = uint8_t(std::max(pad.native_low, pad.pulse_low) >> 8);
  param.small_motor = uint8_t(std::max(pad.native_high, pad.pulse_high) >> 8);
  scePadSetVibration(pad.handle, &param);
  pad.sent_large = param.large_motor;
  pad.sent_small = param.small_motor;
}

// (under mutex_, with the game's read of this controller)
void Ps5InputDriver::ApplyFeedback(uint32_t user_index, Pad& pad) {
  namespace sony = rex::input::sony;
  const uint64_t now = SDL_GetTicks();
  if (now < pad.next_feedback_ms) return;
  pad.next_feedback_ms = now + 8;
  if (sony::GetService().ReadDevice(user_index).generation != pad.generation) return;
  const sony::Options options = sony::GetOptions();
  const sony::Output out = sony::GetService().Compose(user_index, now, options);
  const int32_t handle = pad.handle;
  // rumble: the pulses, when they start and stop
  const uint16_t low = out.active ? out.low_motor : 0, high = out.active ? out.high_motor : 0;
  if (low != pad.pulse_low || high != pad.pulse_high) {
    pad.pulse_low = low, pad.pulse_high = high;
    SendVibration(pad);
  }
  // the light bar: the game's colour (police lights while wanted), else the system's
  if (out.active && out.lighting) {
    if (!pad.light_set || out.color != pad.color) {
      const ScePadLightBarParam light{out.color[0], out.color[1], out.color[2], 0};
      if (scePadSetLightBar(handle, &light) == 0) pad.light_set = true, pad.color = out.color;
    }
  } else if (pad.light_set) {
    scePadResetLightBar(handle);
    pad.light_set = false;
  }
  // the triggers
  if (pad.trigger_failures >= 3) return;
  ScePadTriggerEffectParam param{};
  param.trigger_mask = 1 | 2;
  FillTrigger(param.l2, out.active ? out.left_feel : sony::TriggerFeel{});
  FillTrigger(param.r2, out.active ? out.right_feel : sony::TriggerFeel{});
  const bool off = param.l2.mode == 0 && param.r2.mode == 0;
  if ((!pad.triggers_set && off) ||
      std::memcmp(&param, pad.trigger_param.data(), sizeof(param)) == 0)
    return;
  const int result = scePadSetTriggerEffect(handle, &param);
  if (result == 0) {
    std::memcpy(pad.trigger_param.data(), &param, sizeof(param));
    pad.triggers_set = !off;
    pad.trigger_failures = 0;
  } else if (++pad.trigger_failures >= 3) {
    REXLOG_WARN("PS5 input: adaptive triggers refused ({:#x}); off for this controller", uint32_t(result));
    sony::GetService().DisableTriggers(user_index, pad.generation);
  }
}

// Button presses and releases as keystroke events, one per call (the XInput
// button field, then the triggers as buttons), like the SDL driver's.
X_RESULT Ps5InputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if (!out_keystroke) return X_ERROR_BAD_ARGUMENTS;
  static constexpr rex::ui::VirtualKey kVk[18] = {
      rex::ui::VirtualKey::kXInputPadDpadUp,     rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,   rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress, rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,  rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone,                rex::ui::VirtualKey::kNone,
      rex::ui::VirtualKey::kXInputPadA,          rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,          rex::ui::VirtualKey::kXInputPadY,
      rex::ui::VirtualKey::kXInputPadLTrigger,   rex::ui::VirtualKey::kXInputPadRTrigger,
  };
  std::lock_guard<std::mutex> lock(mutex_);
  const bool any = user_index == 0xFF;
  bool connected = false;
  for (uint32_t u = any ? 0 : user_index; u < (any ? kMaxPads : user_index + 1); ++u) {
    X_INPUT_STATE state{};
    if (ReadState(u, is_active(), &state) != X_ERROR_SUCCESS) continue;
    connected = true;
    Pad& pad = pads_[u];
    uint32_t now = state.gamepad.buttons;
    if (state.gamepad.left_trigger > 30) now |= 1u << 16;
    if (state.gamepad.right_trigger > 30) now |= 1u << 17;
    const uint32_t changed = now ^ pad.keystroke_buttons;
    for (uint32_t bit = 0; bit < 18; ++bit) {
      if (!(changed & (1u << bit)) || kVk[bit] == rex::ui::VirtualKey::kNone) continue;
      const bool down = (now >> bit) & 1;
      pad.keystroke_buttons ^= 1u << bit;
      out_keystroke->virtual_key = uint16_t(kVk[bit]);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = uint8_t(u);
      out_keystroke->hid_code = 0;
      out_keystroke->flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      return X_ERROR_SUCCESS;
    }
    pad.keystroke_buttons = now;
  }
  return connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace rex::input::ps5
