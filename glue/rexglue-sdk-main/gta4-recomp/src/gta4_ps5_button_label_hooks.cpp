#include "gta4_ps5_button_label_policy.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>

#include "gta4_init.h"

#if REX_PLATFORM_PS5

REXCVAR_DEFINE_BOOL(gta4_ps5_button_labels, false, "GTA IV/Frontend",
                    "Show PS BUTTON in place of the Xbox Guide label in control layouts");

namespace {

constexpr uint32_t kStockLabelReadBytes = 12;  // "XBOX GUIDE" plus its terminator

std::atomic<uint32_t> g_ps_button_label{0};
std::atomic<bool> g_ps_button_label_failed{false};
std::atomic<bool> g_ps_button_label_logged{false};

bool IsGuestSpanValid(uint32_t address, std::size_t size) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  if (!memory || address == 0 || size == 0 || size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  if (end > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const auto* first_heap = memory->LookupHeap(address);
  return first_heap != nullptr && first_heap == memory->LookupHeap(static_cast<uint32_t>(end));
}

bool ReadStockLabel(uint8_t* base, uint32_t address, char (&out)[kStockLabelReadBytes]) {
  out[0] = '\0';
  if (!IsGuestSpanValid(address, kStockLabelReadBytes)) {
    return false;
  }
  for (uint32_t index = 0; index < kStockLabelReadBytes; ++index) {
    const char value = static_cast<char>(REX_LOAD_U8(address + index));
    out[index] = value;
    if (value == '\0') {
      return true;
    }
  }
  out[kStockLabelReadBytes - 1] = '\0';
  return true;
}

uint32_t GuestPsButtonLabel(PPCContext& parent, uint8_t* base) {
  uint32_t label = g_ps_button_label.load(std::memory_order_acquire);
  if (label != 0 || g_ps_button_label_failed.load(std::memory_order_relaxed)) {
    return label;
  }

  constexpr uint32_t kLabelBytes =
      sizeof(gta4::frontend_labels::kPlayStationButtonLabel);
  PPCContext nested_ctx = parent;
  nested_ctx.r3.u64 = kLabelBytes;
  __imp__sub_821B3510(nested_ctx, base);
  const uint32_t allocation = nested_ctx.r3.u32;
  if (allocation == 0 || !IsGuestSpanValid(allocation, kLabelBytes)) {
    if (allocation != 0) {
      PPCContext free_ctx = parent;
      free_ctx.r3.u64 = allocation;
      __imp__sub_821B3560(free_ctx, base);
    }
    g_ps_button_label_failed.store(true, std::memory_order_relaxed);
    return 0;
  }

  std::memcpy(REX_RAW_ADDR(allocation), gta4::frontend_labels::kPlayStationButtonLabel,
              kLabelBytes);
  uint32_t expected = 0;
  if (g_ps_button_label.compare_exchange_strong(expected, allocation,
                                                std::memory_order_acq_rel)) {
    return allocation;
  }

  // Another lookup allocated the shared label first; free this copy.
  PPCContext free_ctx = parent;
  free_ctx.r3.u64 = allocation;
  __imp__sub_821B3560(free_ctx, base);
  return expected;
}

}  // namespace

// Single choke point for GXT label resolution. The stock resolver returns a
// plain guest string, so a lookup that yields the guide text can be pointed at
// the PS button label instead. Keys and text are both checked so unrelated
// lookups (including missing keys that fall back to the empty global string)
// stay untouched.
extern "C" void sub_8221F840(PPCContext& ctx, uint8_t* base) {
  const uint32_t key_hash = ctx.r4.u32;
  __imp__sub_8221F840(ctx, base);
  if (!gta4::frontend_labels::IsGuideLabelKey(key_hash)) {
    return;
  }

  char resolved[kStockLabelReadBytes];
  if (!ReadStockLabel(base, ctx.r3.u32, resolved)) {
    return;
  }
  if (!gta4::frontend_labels::ShouldReplaceGuideLabel(
          REXCVAR_GET(gta4_ps5_button_labels), key_hash, resolved)) {
    return;
  }

  const uint32_t label = GuestPsButtonLabel(ctx, base);
  if (label == 0) {
    return;
  }
  if (!g_ps_button_label_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("GTA IV PS button label: guide lookup {:08X} now resolves to '{}'", key_hash,
                gta4::frontend_labels::kPlayStationButtonLabel);
  }
  ctx.r3.u64 = label;
}

#endif  // REX_PLATFORM_PS5
