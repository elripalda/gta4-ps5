// Host-side tests for the F02 Return to Hub Game page row and its F08 position.
//
// These exercise the pure planning/composition rules that the guest hook uses:
// where the row goes on each episode's stock Game page (F08: last, directly
// after Auto Save, with Marketplace dropped in TLAD/TBOGT and one record added
// in base GTA IV), the exact option record bytes the retail list widget and
// dispatcher will see, that every other record keeps its bytes and order, that
// each action stays bound to its own record, and the one-shot request hand-off
// to the overlay.
#include <cstdint>
#include <string>
#include <vector>

#include "../../../gta4-recomp/src/ps5/lifecycle/f02_game_menu.h"
#include "../../../gta4-recomp/src/ps5/lifecycle/f02_menu_request.h"

#ifdef GTA4_F02_STANDALONE
#include <cstdio>
#include <stdexcept>

static int g_checks = 0;
static int g_failures = 0;
#define CHECK_F02(...)                                                       \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(__VA_ARGS__)) throw std::runtime_error(#__VA_ARGS__);              \
  } while (false)
#define RUN_F02(fn)                                                          \
  do {                                                                       \
    try {                                                                    \
      fn();                                                                  \
    } catch (const std::exception& error) {                                  \
      std::fprintf(stderr, "FAIL %s: %s\n", #fn, error.what());              \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)
#else
#include <catch2/catch_test_macros.hpp>
#define CHECK_F02(...) REQUIRE((__VA_ARGS__))
#define RUN_F02(fn) fn()
#endif

namespace {

namespace f02 = gta4::ps5::lifecycle::f02;
namespace gm = gta4::ps5::lifecycle::f02::game_menu;

struct TextRow {
  uint8_t action;
  std::string key;
  uint16_t value;
};

std::string KeyAt(const std::vector<uint8_t>& rows, std::size_t index) {
  const std::size_t offset = index * gm::kOptionRecordSize + gm::kOptionLabelOffset;
  std::string key;
  for (std::size_t i = 0; i < gm::kOptionLabelCapacity; ++i) {
    const char value = static_cast<char>(rows[offset + i]);
    if (value == '\0') break;
    key.push_back(value);
  }
  return key;
}

uint8_t ActionAt(const std::vector<uint8_t>& rows, std::size_t index) {
  return rows[index * gm::kOptionRecordSize + gm::kOptionActionOffset];
}

uint16_t ValueAt(const std::vector<uint8_t>& rows, std::size_t index) {
  const std::size_t offset = index * gm::kOptionRecordSize + gm::kOptionValueOffset;
  return static_cast<uint16_t>((rows[offset] << 8) | rows[offset + 1]);
}

std::vector<uint8_t> StockBytes(const std::vector<TextRow>& rows) {
  std::vector<uint8_t> bytes(rows.size() * gm::kOptionRecordSize, 0);
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const std::size_t offset = index * gm::kOptionRecordSize;
    bytes[offset + gm::kOptionActionOffset] = rows[index].action;
    for (std::size_t i = 0; i < rows[index].key.size(); ++i) {
      bytes[offset + gm::kOptionLabelOffset + i] =
          static_cast<uint8_t>(rows[index].key[i]);
    }
    bytes[offset + gm::kOptionValueOffset] =
        static_cast<uint8_t>(rows[index].value >> 8);
    bytes[offset + gm::kOptionValueOffset + 1] =
        static_cast<uint8_t>(rows[index].value & 0xFF);
  }
  return bytes;
}

std::vector<uint8_t> Compose(const std::vector<TextRow>& rows, gm::Plan& plan) {
  std::vector<gm::RowInfo> info;
  info.reserve(rows.size());
  for (const TextRow& row : rows) {
    info.push_back({.action = row.action, .key = row.key, .value = row.value});
  }
  plan = gm::PlanGameMenu(info.data(), info.size());
  if (!plan.valid) return {};
  const std::vector<uint8_t> stock = StockBytes(rows);
  std::vector<uint8_t> output(plan.output_count * gm::kOptionRecordSize, 0);
  if (!gm::ComposeGameMenu(stock.data(), rows.size(), output.data(), plan)) {
    return {};
  }
  return output;
}

// The shipped TLAD/TBOGT Game page.
std::vector<TextRow> EflcGamePage() {
  return {
      {gm::kMenuOptionJump, "MO_LOA", 14},
      {gm::kMenuOptionJump, "MO_NEW", 13},
      {gm::kMenuOptionJump, std::string(gm::kMarketplaceKey), 16},
      {1, "MO_AUTOSAV", 5},
      {gm::kEndOfMenuOptions, "", 0},
  };
}

// The shipped base GTA IV (v8) Game page.
std::vector<TextRow> BaseGamePage() {
  return {
      {gm::kMenuOptionJump, "MO_LOA", 14},
      {gm::kMenuOptionJump, "MO_NEW", 13},
      {1, "MO_AUTOSAV", 5},
      {gm::kEndOfMenuOptions, "", 0},
  };
}

// ---- F08 helpers -----------------------------------------------------------

// The keys of the rows the retail list shows (everything before END), in order.
std::vector<std::string> VisibleKeys(const std::vector<uint8_t>& output, const gm::Plan& plan) {
  std::vector<std::string> keys;
  for (std::size_t index = 0; index < plan.output_count; ++index) {
    if (ActionAt(output, index) == gm::kEndOfMenuOptions) break;
    keys.push_back(KeyAt(output, index));
  }
  return keys;
}

// What activating a published Game page record does. This mirrors the retail
// dispatcher's contract that is pinned in f02_game_menu.h (sub_82258388 reads
// the record's action byte and value@18; the F02 hook intercepts only the
// sentinel jump value). The row INDEX is never an input.
enum class Outcome { kJumpLoad, kJumpNewGame, kToggleAutosave, kReturnToHub, kOther, kEnd };

Outcome Activate(const std::vector<uint8_t>& rows, std::size_t index) {
  const uint8_t action = ActionAt(rows, index);
  const uint16_t value = ValueAt(rows, index);
  if (action == gm::kEndOfMenuOptions) return Outcome::kEnd;
  if (gm::IsReturnToHubActivation(gm::kPauseMenuIndex, gm::kGameScreen, action, value)) {
    return Outcome::kReturnToHub;
  }
  if (action == gm::kMenuOptionJump && value == 14) return Outcome::kJumpLoad;
  if (action == gm::kMenuOptionJump && value == 13) return Outcome::kJumpNewGame;
  if (action == 1 && value == 5) return Outcome::kToggleAutosave;
  return Outcome::kOther;
}

std::vector<uint8_t> Record(const std::vector<uint8_t>& rows, std::size_t index) {
  return std::vector<uint8_t>(rows.begin() + index * gm::kOptionRecordSize,
                              rows.begin() + (index + 1) * gm::kOptionRecordSize);
}

struct TitlePage {
  const char* title;
  std::vector<TextRow> (*page)();
};

// ---- F08: ordering ---------------------------------------------------------

void EflcPageHasAutoSaveWhereReturnWasAndReturnLast() {
  gm::Plan plan;
  const std::vector<uint8_t> output = Compose(EflcGamePage(), plan);
  CHECK_F02(plan.valid);
  CHECK_F02(plan.replaced_marketplace);
  CHECK_F02(plan.dropped_stock_index == 2);
  CHECK_F02(plan.output_count == 5);
  CHECK_F02(plan.return_index == 3);
  CHECK_F02(plan.end_index == 4);
  CHECK_F02(output.size() == 5 * gm::kOptionRecordSize);
  // Load, New Game, Auto Save, Return to Hub, END.
  CHECK_F02(KeyAt(output, 0) == "MO_LOA");
  CHECK_F02(KeyAt(output, 1) == "MO_NEW");
  CHECK_F02(KeyAt(output, 2) == gm::kAutosaveKey);
  CHECK_F02(KeyAt(output, 3) == gm::kReturnToHubKey);
  CHECK_F02(ActionAt(output, 3) == gm::kMenuOptionJump);
  CHECK_F02(ValueAt(output, 3) == gm::kReturnToHubValue);
  CHECK_F02(ActionAt(output, 4) == gm::kEndOfMenuOptions);
  // Marketplace is still gone from the published page.
  for (std::size_t index = 0; index < plan.output_count; ++index) {
    CHECK_F02(KeyAt(output, index) != gm::kMarketplaceKey);
  }
}

void BasePageHasAutoSaveWhereReturnWasAndReturnLast() {
  gm::Plan plan;
  const std::vector<uint8_t> output = Compose(BaseGamePage(), plan);
  CHECK_F02(plan.valid);
  CHECK_F02(!plan.replaced_marketplace);
  CHECK_F02(plan.dropped_stock_index == gm::kNoRow);
  CHECK_F02(plan.output_count == 5);
  CHECK_F02(plan.return_index == 3);
  CHECK_F02(plan.end_index == 4);
  // Load, New Game, Auto Save, Return to Hub, END.
  CHECK_F02(KeyAt(output, 0) == "MO_LOA");
  CHECK_F02(KeyAt(output, 1) == "MO_NEW");
  CHECK_F02(KeyAt(output, 2) == gm::kAutosaveKey);
  CHECK_F02(KeyAt(output, 3) == gm::kReturnToHubKey);
  CHECK_F02(ActionAt(output, 4) == gm::kEndOfMenuOptions);
}

void AutoSaveSitsAtRowTwoAndReturnIsTheBottomRowInAllThreeTitles() {
  const TitlePage titles[] = {
      {"GTA IV", &BaseGamePage},
      {"TLAD", &EflcGamePage},
      {"TBOGT", &EflcGamePage},
  };
  for (const TitlePage& title : titles) {
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(title.page(), plan);
    CHECK_F02(plan.valid);
    const std::vector<std::string> visible = VisibleKeys(output, plan);
    CHECK_F02(visible.size() == 4);
    CHECK_F02(visible[2] == gm::kAutosaveKey);   // where Return to Hub used to be
    CHECK_F02(visible.back() == gm::kReturnToHubKey);  // bottommost option
    CHECK_F02(plan.return_index == visible.size() - 1);
    // The page ends with exactly one END record directly after Return to Hub.
    CHECK_F02(ActionAt(output, plan.end_index) == gm::kEndOfMenuOptions);
    CHECK_F02(plan.end_index == plan.return_index + 1);
  }
}

void NoOtherItemChangesOrderOrBytes() {
  for (const auto& page : {BaseGamePage(), EflcGamePage()}) {
    const std::vector<uint8_t> stock = StockBytes(page);
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(page, plan);
    CHECK_F02(plan.valid);
    // The published page minus the Return record, in order, equals the stock
    // page minus Marketplace, byte for byte (Load, New Game, Auto Save, END).
    std::vector<std::vector<uint8_t>> expected;
    for (std::size_t index = 0; index < page.size(); ++index) {
      if (page[index].key == gm::kMarketplaceKey) continue;
      expected.push_back(Record(stock, index));
    }
    std::vector<std::vector<uint8_t>> actual;
    for (std::size_t index = 0; index < plan.output_count; ++index) {
      if (index == plan.return_index) continue;
      actual.push_back(Record(output, index));
    }
    CHECK_F02(actual == expected);
    // The Return record is the generated sentinel record and appears once.
    const gm::OptionRecord row = gm::MakeReturnToHubRow();
    CHECK_F02(Record(output, plan.return_index) ==
              std::vector<uint8_t>(row.bytes.begin(), row.bytes.end()));
    std::size_t returns = 0;
    for (std::size_t index = 0; index < plan.output_count; ++index) {
      if (KeyAt(output, index) == gm::kReturnToHubKey) ++returns;
    }
    CHECK_F02(returns == 1);
    // Marketplace never reappears; the relative order Load < New Game < Auto Save holds.
    CHECK_F02(KeyAt(output, 0) == "MO_LOA" && KeyAt(output, 1) == "MO_NEW" &&
              KeyAt(output, 2) == gm::kAutosaveKey);
  }
}

void LayoutVariantsStillPutReturnLastAndKeepEveryRecord() {
  // Marketplace AFTER Auto Save.
  {
    const std::vector<TextRow> rows = {
        {gm::kMenuOptionJump, "MO_LOA", 14},
        {gm::kMenuOptionJump, "MO_NEW", 13},
        {1, "MO_AUTOSAV", 5},
        {gm::kMenuOptionJump, std::string(gm::kMarketplaceKey), 16},
        {gm::kEndOfMenuOptions, "", 0},
    };
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(rows, plan);
    CHECK_F02(plan.valid && plan.return_index == 3 && plan.end_index == 4);
    CHECK_F02(KeyAt(output, 2) == gm::kAutosaveKey);
    CHECK_F02(KeyAt(output, 3) == gm::kReturnToHubKey);
    CHECK_F02(ActionAt(output, 4) == gm::kEndOfMenuOptions);
  }
  // EFLC page without an Auto Save row: Return goes directly before END.
  {
    const std::vector<TextRow> rows = {
        {gm::kMenuOptionJump, "MO_LOA", 14},
        {gm::kMenuOptionJump, "MO_NEW", 13},
        {gm::kMenuOptionJump, std::string(gm::kMarketplaceKey), 16},
        {gm::kEndOfMenuOptions, "", 0},
    };
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(rows, plan);
    CHECK_F02(plan.valid && plan.output_count == 4 && plan.return_index == 2);
    CHECK_F02(KeyAt(output, 1) == "MO_NEW");
    CHECK_F02(KeyAt(output, 2) == gm::kReturnToHubKey);
    CHECK_F02(ActionAt(output, 3) == gm::kEndOfMenuOptions);
  }
  // Base page without New Game (older or localized page).
  {
    const std::vector<TextRow> rows = {
        {gm::kMenuOptionJump, "MO_LOA", 14},
        {1, "MO_AUTOSAV", 5},
        {gm::kEndOfMenuOptions, "", 0},
    };
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(rows, plan);
    CHECK_F02(plan.valid && plan.return_index == 2 && plan.output_count == 4);
    CHECK_F02(KeyAt(output, 1) == gm::kAutosaveKey);
    CHECK_F02(KeyAt(output, 2) == gm::kReturnToHubKey);
    CHECK_F02(ActionAt(output, 3) == gm::kEndOfMenuOptions);
  }
}

// ---- F08: actions stay bound to their own records --------------------------

void EachActionStaysBoundToItsOwnRecord() {
  const TitlePage titles[] = {
      {"GTA IV", &BaseGamePage},
      {"TLAD", &EflcGamePage},
      {"TBOGT", &EflcGamePage},
  };
  for (const TitlePage& title : titles) {
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(title.page(), plan);
    CHECK_F02(plan.valid);
    int returns = 0, autosaves = 0, loads = 0, news = 0;
    for (std::size_t index = 0; index < plan.output_count; ++index) {
      const std::string key = KeyAt(output, index);
      const Outcome outcome = Activate(output, index);
      if (key == gm::kReturnToHubKey) {
        CHECK_F02(outcome == Outcome::kReturnToHub);
        ++returns;
      } else if (key == gm::kAutosaveKey) {
        // Selecting Auto Save must never request a Return to Hub, and keeps its
        // original toggle action and setting id.
        CHECK_F02(outcome == Outcome::kToggleAutosave);
        CHECK_F02(ActionAt(output, index) == 1 && ValueAt(output, index) == 5);
        ++autosaves;
      } else if (key == "MO_LOA") {
        CHECK_F02(outcome == Outcome::kJumpLoad);
        ++loads;
      } else if (key == "MO_NEW") {
        CHECK_F02(outcome == Outcome::kJumpNewGame);
        ++news;
      } else {
        CHECK_F02(outcome == Outcome::kEnd);
      }
    }
    CHECK_F02(returns == 1 && autosaves == 1 && loads == 1 && news == 1);
    // The swap did not exchange the actions: Return's row is not the Auto Save
    // record and vice versa.
    CHECK_F02(Record(output, plan.return_index) != Record(output, 2));
    CHECK_F02(Activate(output, 2) == Outcome::kToggleAutosave);
    CHECK_F02(Activate(output, 3) == Outcome::kReturnToHub);
  }
}

// ---- F08: controller navigation --------------------------------------------

// The retail list walks the option array in order (D-pad/stick down is +1,
// up is -1, wrapping at the ends, END is not a row). The retail widget is not
// run here; this models the traversal the published order implies.
std::size_t Down(std::size_t row, std::size_t visible) { return (row + 1) % visible; }
std::size_t Up(std::size_t row, std::size_t visible) { return (row + visible - 1) % visible; }

void NavigationVisitsRowsInThePublishedOrderAndWraps() {
  const TitlePage titles[] = {
      {"GTA IV", &BaseGamePage},
      {"TLAD", &EflcGamePage},
      {"TBOGT", &EflcGamePage},
  };
  for (const TitlePage& title : titles) {
    gm::Plan plan;
    const std::vector<uint8_t> output = Compose(title.page(), plan);
    CHECK_F02(plan.valid);
    const std::vector<std::string> visible = VisibleKeys(output, plan);
    const std::size_t n = visible.size();
    CHECK_F02(n == 4);
    // From the top: Load -> New Game -> Auto Save -> Return to Hub -> (wrap) Load.
    std::size_t row = 0;
    const char* order[] = {"MO_LOA", "MO_NEW", "MO_AUTOSAV", "F02_RETURN_HUB", "MO_LOA"};
    for (const char* expected : order) {
      CHECK_F02(visible[row] == expected);
      row = Down(row, n);
    }
    // Up from Load wraps to Return to Hub (the bottom row); Up from Return lands on Auto Save.
    CHECK_F02(visible[Up(0, n)] == gm::kReturnToHubKey);
    CHECK_F02(visible[Up(n - 1, n)] == gm::kAutosaveKey);
    // Down from Auto Save lands on Return to Hub; every row is reachable once.
    CHECK_F02(visible[Down(2, n)] == gm::kReturnToHubKey);
    std::vector<bool> seen(n, false);
    row = 0;
    for (std::size_t step = 0; step < n; ++step) {
      seen[row] = true;
      row = Down(row, n);
    }
    for (bool value : seen) CHECK_F02(value);
    // Activating the row under the cursor at each position dispatches that row's own action.
    CHECK_F02(Activate(output, 2) == Outcome::kToggleAutosave);
    CHECK_F02(Activate(output, 3) == Outcome::kReturnToHub);
  }
}

// ---- F08: composition refuses stale or inconsistent plans -------------------

void ComposeRejectsAStalePlan() {
  const std::vector<TextRow> rows = EflcGamePage();
  const std::vector<uint8_t> stock = StockBytes(rows);
  gm::Plan plan;
  Compose(rows, plan);
  CHECK_F02(plan.valid);
  std::vector<uint8_t> output(plan.output_count * gm::kOptionRecordSize, 0);
  // Wrong stock count.
  CHECK_F02(!gm::ComposeGameMenu(stock.data(), rows.size() - 1, output.data(), plan));
  // Return index inconsistent with the insertion point.
  gm::Plan bad = plan;
  bad.return_index = plan.return_index - 1;
  CHECK_F02(!gm::ComposeGameMenu(stock.data(), rows.size(), output.data(), bad));
  // Insertion point beyond the stock array.
  bad = plan;
  bad.insert_before_stock = rows.size() + 1;
  CHECK_F02(!gm::ComposeGameMenu(stock.data(), rows.size(), output.data(), bad));
  // Marketplace index out of range / dropped row claimed on an insertion plan.
  bad = plan;
  bad.dropped_stock_index = rows.size();
  CHECK_F02(!gm::ComposeGameMenu(stock.data(), rows.size(), output.data(), bad));
  gm::Plan base_plan;
  const std::vector<TextRow> base = BaseGamePage();
  Compose(base, base_plan);
  const std::vector<uint8_t> base_stock = StockBytes(base);
  std::vector<uint8_t> base_out(base_plan.output_count * gm::kOptionRecordSize, 0);
  bad = base_plan;
  bad.dropped_stock_index = 0;
  CHECK_F02(!gm::ComposeGameMenu(base_stock.data(), base.size(), base_out.data(), bad));
  // The good plans still compose.
  CHECK_F02(gm::ComposeGameMenu(stock.data(), rows.size(), output.data(), plan));
  CHECK_F02(gm::ComposeGameMenu(base_stock.data(), base.size(), base_out.data(), base_plan));
}

void RejectsAlreadyInstalledPage() {
  std::vector<TextRow> rows = BaseGamePage();
  rows.emplace_back(TextRow{gm::kMenuOptionJump, std::string(gm::kReturnToHubKey),
                            gm::kReturnToHubValue});
  std::vector<gm::RowInfo> info;
  for (const TextRow& row : rows) {
    info.push_back({.action = row.action, .key = row.key, .value = row.value});
  }
  CHECK_F02(!gm::PlanGameMenu(info.data(), info.size()).valid);
}

void RejectsMalformedPages() {
  CHECK_F02(!gm::PlanGameMenu(nullptr, 0).valid);
  // No END row.
  const std::vector<TextRow> no_end = {
      {gm::kMenuOptionJump, "MO_LOA", 14},
      {gm::kMenuOptionJump, "MO_NEW", 13},
  };
  gm::Plan plan;
  CHECK_F02(Compose(no_end, plan).empty());
  CHECK_F02(!plan.valid);
  // No room for the extra base-IV row.
  std::vector<TextRow> full;
  for (int i = 0; i < static_cast<int>(gm::kRetailListSlotCapacity) - 1; ++i) {
    full.push_back({gm::kMenuOptionJump, "MO_X", 1});
  }
  full.push_back({gm::kEndOfMenuOptions, "", 0});
  CHECK_F02(Compose(full, plan).empty());
  CHECK_F02(!plan.valid);
}

void RowRecordMatchesTheRetailLayout() {
  const gm::OptionRecord row = gm::MakeReturnToHubRow();
  CHECK_F02(row.bytes[0] == gm::kMenuOptionJump);
  CHECK_F02(row.bytes[gm::kOptionValueOffset] == 0x7F);
  CHECK_F02(row.bytes[gm::kOptionValueOffset + 1] == 0xFE);
  CHECK_F02(row.bytes[gm::kOptionScalerOffset] == 0);
  CHECK_F02(row.bytes[gm::kOptionDisplayValueOffset] == 0);
  uint8_t action = 0;
  std::string_view key;
  uint16_t value = 0;
  uint8_t scaler = 0xFF;
  uint8_t display = 0xFF;
  CHECK_F02(gm::ParseReturnToHubRow(row.bytes.data(), action, key, value, scaler, display));
  CHECK_F02(action == gm::kMenuOptionJump);
  CHECK_F02(key == gm::kReturnToHubKey);
  CHECK_F02(value == gm::kReturnToHubValue);
  CHECK_F02(scaler == 0);
  CHECK_F02(display == 0);
  // The internal key fits the 16-byte inline field and the label is the text
  // the retail widget will draw through the label resolver.
  CHECK_F02(gm::FitsInlineKey(gm::kReturnToHubKey.size()));
  CHECK_F02(gm::kReturnToHubLabel == "Return to Hub");
  CHECK_F02(gm::kReturnToHubKey.size() < gm::kOptionLabelCapacity);
}

void ActivationIsThePauseMenuGamePageOnly() {
  CHECK_F02(gm::IsReturnToHubKey(gm::kReturnToHubKey));
  CHECK_F02(!gm::IsReturnToHubKey("MO_NEW"));
  CHECK_F02(gm::IsReturnToHubActivation(gm::kPauseMenuIndex, gm::kGameScreen,
                                        gm::kMenuOptionJump, gm::kReturnToHubValue));
  CHECK_F02(!gm::IsReturnToHubActivation(3, gm::kGameScreen, gm::kMenuOptionJump,
                                         gm::kReturnToHubValue));
  CHECK_F02(!gm::IsReturnToHubActivation(gm::kPauseMenuIndex, 8, gm::kMenuOptionJump,
                                         gm::kReturnToHubValue));
  CHECK_F02(!gm::IsReturnToHubActivation(gm::kPauseMenuIndex, gm::kGameScreen, 1,
                                         gm::kReturnToHubValue));
  CHECK_F02(!gm::IsReturnToHubActivation(gm::kPauseMenuIndex, gm::kGameScreen,
                                         gm::kMenuOptionJump, 14));
}

void MenuRequestIsConsumedOnce() {
  f02::ResetReturnRequest();
  CHECK_F02(!f02::ConsumeReturnRequest());
  f02::RequestReturnFromMenu();
  f02::RequestReturnFromMenu();
  CHECK_F02(f02::ConsumeReturnRequest());
  CHECK_F02(!f02::ConsumeReturnRequest());
  f02::RequestReturnFromMenu();
  f02::ResetReturnRequest();
  CHECK_F02(!f02::ConsumeReturnRequest());
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(EflcPageHasAutoSaveWhereReturnWasAndReturnLast);
  RUN_F02(BasePageHasAutoSaveWhereReturnWasAndReturnLast);
  RUN_F02(AutoSaveSitsAtRowTwoAndReturnIsTheBottomRowInAllThreeTitles);
  RUN_F02(NoOtherItemChangesOrderOrBytes);
  RUN_F02(LayoutVariantsStillPutReturnLastAndKeepEveryRecord);
  RUN_F02(EachActionStaysBoundToItsOwnRecord);
  RUN_F02(NavigationVisitsRowsInThePublishedOrderAndWraps);
  RUN_F02(ComposeRejectsAStalePlan);
  RUN_F02(RejectsAlreadyInstalledPage);
  RUN_F02(RejectsMalformedPages);
  RUN_F02(RowRecordMatchesTheRetailLayout);
  RUN_F02(ActivationIsThePauseMenuGamePageOnly);
  RUN_F02(MenuRequestIsConsumedOnce);
  std::printf("f02_game_menu: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F08 TLAD/TBOGT page: Auto Save at row 2, Return to Hub last") {
  RUN_F02(EflcPageHasAutoSaveWhereReturnWasAndReturnLast);
}
TEST_CASE("F08 base GTA IV page: Auto Save at row 2, Return to Hub last") {
  RUN_F02(BasePageHasAutoSaveWhereReturnWasAndReturnLast);
}
TEST_CASE("F08 Auto Save row 2 and Return to Hub bottom in GTA IV, TLAD and TBOGT") {
  RUN_F02(AutoSaveSitsAtRowTwoAndReturnIsTheBottomRowInAllThreeTitles);
}
TEST_CASE("F08 no other Game page item changes order or bytes") {
  RUN_F02(NoOtherItemChangesOrderOrBytes);
}
TEST_CASE("F08 layout variants keep every record and put Return to Hub last") {
  RUN_F02(LayoutVariantsStillPutReturnLastAndKeepEveryRecord);
}
TEST_CASE("F08 each action stays bound to its own record") {
  RUN_F02(EachActionStaysBoundToItsOwnRecord);
}
TEST_CASE("F08 navigation visits rows in the published order and wraps") {
  RUN_F02(NavigationVisitsRowsInThePublishedOrderAndWraps);
}
TEST_CASE("F08 composition rejects a stale plan") {
  RUN_F02(ComposeRejectsAStalePlan);
}
TEST_CASE("F02 Game page rejects an already installed page") {
  RUN_F02(RejectsAlreadyInstalledPage);
}
TEST_CASE("F02 Game page rejects malformed stock pages") {
  RUN_F02(RejectsMalformedPages);
}
TEST_CASE("F02 Game page row uses the retail option layout") {
  RUN_F02(RowRecordMatchesTheRetailLayout);
}
TEST_CASE("F02 Game page activation is pause menu screen 0 only") {
  RUN_F02(ActivationIsThePauseMenuGamePageOnly);
}
TEST_CASE("F02 Game page request is consumed once") {
  RUN_F02(MenuRequestIsConsumedOnce);
}
#endif
