// F02 Return to Hub: the retail Pause Menu -> Game page row.
//
// The row is a normal frontend menu option: it is stored in the same 22-byte
// option record the parser fills from frontend_menus.xml, drawn by the retail
// list widget and reached with the retail D-pad/stick navigation. Only two
// things differ from a stock jump row: its label key resolves to the host's
// text, and its screen value is a sentinel the F02 dispatcher intercepts
// instead of switching screens.
//
// v8 evidence (generated code and the shipped XML):
//   - screen 0 is SCR_GAME; its descriptor is 0x831DAE28 + 0*24, options
//     pointer at +16, count at +20, capacity at +22
//   - option records are 22 bytes: action @0, 16-byte key @1, value @18,
//     scaler @20, displayValue @21
//   - sub_82258388 (row action dispatcher) treats action 10 as "jump to the
//     screen in value@18" via sub_82256D08, with no validation of the target
//   - the base GTA IV Game page is Load, New Game, Autosave, END; TLAD and
//     TBOGT add Marketplace between New Game and Autosave
//
// F08 (menu position swap): the published page puts Return to Hub LAST, after
// Auto Save, instead of before it. Every option is an independent record that
// carries its own action, key and value, and the retail dispatcher acts on
// those bytes, never on the row index, so moving records changes only where
// they are drawn and reached. The published pages are:
//   base GTA IV      stock Load, New Game, Autosave, END
//                    -> Load, New Game, Autosave, Return to Hub, END
//   TLAD and TBOGT   stock Load, New Game, Marketplace, Autosave, END
//                    -> Load, New Game, Autosave, Return to Hub, END
// (Marketplace is still dropped, as before F08.) Auto Save therefore sits where
// Return to Hub used to be (row 2) and Return to Hub is the bottom option.
//
// The host never edits a game archive and never writes into the stock option
// array: it publishes a cloned array for the Game screen and restores/reuses
// the stock one when the frontend menus are reloaded (the loader frees the
// published array itself).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace gta4::ps5::lifecycle::f02::game_menu {

// Screen 0 is SCR_GAME: the pause menu's Game tab. The pause menu itself is
// CPauseMenu index 0 (the frontend menu is index 3).
constexpr uint32_t kGameScreen = 0;
constexpr uint32_t kPauseMenuIndex = 0;

// Verified in sub_82258388: case 7 of (action - 3) is action 10, the generic
// jump that reads the target screen from value@18.
constexpr uint8_t kMenuOptionJump = 10;
constexpr uint8_t kEndOfMenuOptions = 36;

constexpr std::size_t kOptionRecordSize = 22;
constexpr std::size_t kOptionActionOffset = 0;
constexpr std::size_t kOptionLabelOffset = 1;
constexpr std::size_t kOptionLabelCapacity = 16;
constexpr std::size_t kOptionValueOffset = 18;
constexpr std::size_t kOptionScalerOffset = 20;
constexpr std::size_t kOptionDisplayValueOffset = 21;
constexpr std::size_t kRetailListSlotCapacity = 20;

// The internal key is resolved by the F02 label hook; the label is what the
// retail list widget draws. The key fits the 16-byte inline field.
constexpr std::string_view kReturnToHubKey = "F02_RETURN_HUB";
constexpr std::string_view kReturnToHubLabel = "Return to Hub";
constexpr std::string_view kMarketplaceKey = "MO_MARKETPLACE";
constexpr std::string_view kNewGameKey = "MO_NEW";
// The stock Auto Save row (action 1, value = the setting id). Its record is
// copied unchanged; only its position in the published array can differ.
constexpr std::string_view kAutosaveKey = "MO_AUTOSAV";

// The native confirmation is the retail pause-menu message box. Its opener
// copies the key into a 16-byte guest buffer (sub_8224CD88), and the text
// builder resolves that key through sub_8221FD88, so the key must fit inline
// and must differ from the row's key (they resolve to different text).
constexpr std::string_view kReturnToHubConfirmKey = "F02_RTH_CONFIRM";
constexpr std::string_view kReturnToHubDialogText =
    "Are you sure you want to return to the Hub?\nAll unsaved progress will be lost.";
static_assert(kReturnToHubConfirmKey.size() < kOptionLabelCapacity);

// A screen id no stock menu uses; the dispatcher turns it into the host
// request instead of a screen switch. Positive so the signed 16-bit load in
// sub_82258388 keeps it intact.
constexpr uint16_t kReturnToHubValue = 0x7FFE;

constexpr bool FitsInlineKey(std::size_t size) noexcept {
  return size < kOptionLabelCapacity;
}

struct RowInfo {
  uint8_t action = 0;
  std::string_view key;
  uint16_t value = 0;
};

inline constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

struct Plan {
  bool valid = false;
  bool replaced_marketplace = false;
  // Number of option records in the published Game page (the stock count,
  // plus one when base GTA IV has no Marketplace row to replace).
  std::size_t output_count = 0;
  // Where the Return to Hub row goes in the published array (F08: after Auto
  // Save, directly before END).
  std::size_t return_index = 0;
  // Index of the END row in the published array.
  std::size_t end_index = 0;
  // Stock index of the Marketplace row that is dropped (kNoRow when none).
  std::size_t dropped_stock_index = kNoRow;
  // The Return row is emitted immediately before this stock index.
  std::size_t insert_before_stock = 0;
};

// Plan the published Game page. The Return to Hub row is placed immediately
// after the stock Auto Save row (directly before END when there is no Auto Save
// row). TLAD/TBOGT drop the Marketplace row (the row count is unchanged); base
// GTA IV, which has none, gains one record. The stock END row must be present;
// the plan is refused when the array is empty, over the retail list capacity,
// or the same key already exists.
inline Plan PlanGameMenu(const RowInfo* rows, std::size_t count) noexcept {
  Plan plan;
  if (rows == nullptr || count == 0 || count > kRetailListSlotCapacity) {
    return plan;
  }
  std::size_t end = count;
  std::size_t marketplace = count;
  std::size_t autosave = count;
  for (std::size_t index = 0; index < count; ++index) {
    if (rows[index].key == kReturnToHubKey) {
      return plan;  // already installed
    }
    if (end == count && rows[index].action == kEndOfMenuOptions) {
      end = index;
    }
    if (marketplace == count && rows[index].key == kMarketplaceKey) {
      marketplace = index;
    }
    if (autosave == count && rows[index].key == kAutosaveKey) {
      autosave = index;
    }
  }
  if (end == count || end == 0) {
    return plan;  // no END row, or a layout with nothing before it
  }
  // F08: directly after Auto Save; before END when Auto Save is missing (or,
  // defensively, not ahead of END).
  const std::size_t insert_before = (autosave != count && autosave < end) ? autosave + 1 : end;
  if (marketplace != count) {
    // TLAD/TBOGT: Marketplace is dropped, so rows after it shift up by one.
    plan.valid = true;
    plan.replaced_marketplace = true;
    plan.output_count = count;
    plan.dropped_stock_index = marketplace;
    plan.insert_before_stock = insert_before;
    plan.return_index = insert_before - (marketplace < insert_before ? 1 : 0);
    plan.end_index = end - (marketplace < end ? 1 : 0) + 1;
    return plan;
  }
  if (count + 1 > kRetailListSlotCapacity) {
    return plan;
  }
  plan.valid = true;
  plan.replaced_marketplace = false;
  plan.output_count = count + 1;
  plan.dropped_stock_index = kNoRow;
  plan.insert_before_stock = insert_before;
  plan.return_index = insert_before;
  plan.end_index = end + 1;
  return plan;
}

inline bool IsReturnToHubKey(std::string_view key) noexcept {
  return key == kReturnToHubKey;
}

// The row the dispatcher must turn into a host request. The key is checked
// separately against the guest's inline label field.
inline bool IsReturnToHubActivation(uint32_t menu_index, uint32_t screen, uint8_t action,
                                    uint16_t value) noexcept {
  return menu_index == kPauseMenuIndex && screen == kGameScreen &&
         action == kMenuOptionJump && value == kReturnToHubValue;
}

// The published record. Layout matches the guest's big-endian option record;
// writing bytes directly avoids depending on host endianness.
struct OptionRecord {
  std::array<uint8_t, kOptionRecordSize> bytes{};
};

inline OptionRecord MakeReturnToHubRow() noexcept {
  OptionRecord record;
  record.bytes[kOptionActionOffset] = kMenuOptionJump;
  std::memcpy(record.bytes.data() + kOptionLabelOffset, kReturnToHubKey.data(),
              kReturnToHubKey.size());
  record.bytes[kOptionValueOffset] = static_cast<uint8_t>(kReturnToHubValue >> 8);
  record.bytes[kOptionValueOffset + 1] = static_cast<uint8_t>(kReturnToHubValue & 0xFF);
  record.bytes[kOptionScalerOffset] = 0;
  record.bytes[kOptionDisplayValueOffset] = 0;
  return record;
}

// Compose the published Game page from the stock records: every stock record
// is copied unchanged and in its stock order, except Marketplace (dropped in
// EFLC), and the Return row is emitted right after Auto Save (F08). No other
// record moves relative to its neighbours. `destination` must hold
// plan.output_count * kOptionRecordSize bytes. The plan and the stock count
// are cross-checked so a stale plan cannot read past the stock array.
inline bool ComposeGameMenu(const uint8_t* stock, std::size_t stock_count, uint8_t* destination,
                            const Plan& plan) noexcept {
  if (stock == nullptr || destination == nullptr || !plan.valid || plan.output_count == 0 ||
      plan.return_index >= plan.output_count) {
    return false;
  }
  // A replacement keeps the stock count; an insertion adds one record.
  const std::size_t expected_stock =
      plan.replaced_marketplace ? plan.output_count : plan.output_count - 1;
  if (stock_count != expected_stock) {
    return false;
  }
  if (plan.insert_before_stock > stock_count ||
      (plan.replaced_marketplace && plan.dropped_stock_index >= stock_count) ||
      (!plan.replaced_marketplace && plan.dropped_stock_index != kNoRow)) {
    return false;
  }
  const OptionRecord return_row = MakeReturnToHubRow();
  std::size_t out_index = 0;
  bool return_written = false;
  for (std::size_t source = 0; source <= stock_count; ++source) {
    if (source == plan.insert_before_stock) {
      if (out_index != plan.return_index || out_index >= plan.output_count) {
        return false;
      }
      std::memcpy(destination + out_index * kOptionRecordSize, return_row.bytes.data(),
                  kOptionRecordSize);
      ++out_index;
      return_written = true;
    }
    if (source == stock_count) {
      break;
    }
    if (source == plan.dropped_stock_index) {
      continue;  // Marketplace: not offered on the PS5
    }
    if (out_index >= plan.output_count) {
      return false;
    }
    std::memcpy(destination + out_index * kOptionRecordSize, stock + source * kOptionRecordSize,
                kOptionRecordSize);
    ++out_index;
  }
  return return_written && out_index == plan.output_count;
}

// Read back what MakeReturnToHubRow wrote (used by the host tests).
inline bool ParseReturnToHubRow(const uint8_t* record, uint8_t& action,
                                std::string_view& key, uint16_t& value,
                                uint8_t& scaler, uint8_t& display_value) noexcept {
  if (record == nullptr) {
    return false;
  }
  action = record[kOptionActionOffset];
  const char* text = reinterpret_cast<const char*>(record + kOptionLabelOffset);
  std::size_t length = 0;
  while (length < kOptionLabelCapacity && text[length] != '\0') {
    ++length;
  }
  key = std::string_view(text, length);
  value = static_cast<uint16_t>((record[kOptionValueOffset] << 8) |
                                record[kOptionValueOffset + 1]);
  scaler = record[kOptionScalerOffset];
  display_value = record[kOptionDisplayValueOffset];
  return true;
}

}  // namespace gta4::ps5::lifecycle::f02::game_menu
