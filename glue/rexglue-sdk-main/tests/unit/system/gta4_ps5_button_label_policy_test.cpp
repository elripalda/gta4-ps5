#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <iterator>
#include <string_view>

#include "../../../gta4-recomp/src/gta4_ps5_button_label_policy.h"

namespace labels = gta4::frontend_labels;

TEST_CASE("GTA IV PS button label is the exact DualSense spelling",
          "[system][gta4][frontend][ps-button]") {
  CHECK(std::string_view(labels::kPlayStationButtonLabel) == "PS BUTTON");
  CHECK(labels::kPlayStationButtonLabelView == "PS BUTTON");
  CHECK(labels::kPlayStationButtonLabelView.size() == 9);
  CHECK(labels::kPlayStationButtonLabelView.find("XBOX") == std::string_view::npos);
}

TEST_CASE("GTA IV PS button policy covers every stock guide GXT key",
          "[system][gta4][frontend][ps-button]") {
  CHECK(std::size(labels::kGuideLabelKeys) == 6);
  for (const std::uint32_t key : labels::kGuideLabelKeys) {
    REQUIRE(labels::IsGuideLabelKey(key));
    CHECK(labels::ShouldReplaceGuideLabel(true, key, "GUIDE"));
    CHECK(labels::ShouldReplaceGuideLabel(true, key, "XBOX GUIDE"));
  }
  CHECK(labels::IsGuideLabelKey(0x1EAF9417u));
  CHECK(labels::IsGuideLabelKey(0xAAA3DC58u));
  CHECK(labels::IsGuideLabelKey(0xACDB130Du));
  CHECK(labels::IsGuideLabelKey(0x1854A8EBu));
  CHECK(labels::IsGuideLabelKey(0x1A3A3D5Du));
  CHECK(labels::IsGuideLabelKey(0x75D4240Cu));
}

TEST_CASE("GTA IV PS button policy leaves the stock label when disabled",
          "[system][gta4][frontend][ps-button]") {
  for (const std::uint32_t key : labels::kGuideLabelKeys) {
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(false, key, "GUIDE"));
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(false, key, "XBOX GUIDE"));
  }
}

TEST_CASE("GTA IV PS button policy leaves unrelated labels untouched",
          "[system][gta4][frontend][ps-button]") {
  // Another key resolving to guide text (not in the stock GXT) is not the
  // guide label lookup, and the guide keys with other text are reused hashes.
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0x00000000u, "GUIDE"));
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0xAD0259ABu, "ON FOOT"));
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0xAD0259ABu, "GUIDE"));
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0xE8BA3BCDu, "IN VEHICLE"));
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0xE8BA3BCDu, "XBOX GUIDE"));

  for (const std::uint32_t key : labels::kGuideLabelKeys) {
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, key, ""));
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, key, "guide"));
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, key, "GUIDES"));
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, key, "XBOX"));
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, key, "ON FOOT"));
    CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, key, "IN VEHICLE"));
  }
}

TEST_CASE("GTA IV PS button policy stays out of the episode text tables",
          "[system][gta4][frontend][ps-button]") {
  // Sample keys from the public TLAD and TBOGT text indexes; neither episode
  // index carries the guide label, so neither hash may match the policy.
  CHECK_FALSE(labels::IsGuideLabelKey(0x0E1F7C58u));
  CHECK_FALSE(labels::IsGuideLabelKey(0x0CF2FC73u));
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0x0E1F7C58u, "GUIDE"));
  CHECK_FALSE(labels::ShouldReplaceGuideLabel(true, 0x0CF2FC73u, "XBOX GUIDE"));
}
