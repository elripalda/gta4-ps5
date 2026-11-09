#pragma once

#include <cstdint>
#include <string_view>

namespace gta4::frontend_labels {

// The controller-layout label GTA IV ships for the Xbox Guide button.
inline constexpr char kPlayStationButtonLabel[] = "PS BUTTON";
inline constexpr std::string_view kPlayStationButtonLabelView{kPlayStationButtonLabel};

// Base GTA IV GXT keys whose stock value is the Guide button label. The
// values come from the public American GXT text index (ThirteenAG
// GTA-One-Liners, plugins/data/text/GTAIV.json): three keys resolve to GUIDE
// and three to XBOX GUIDE. The episode GXT indexes contain neither value, so
// the episodes resolve the label through the base table or the update override.
inline constexpr std::uint32_t kGuideLabelKeys[] = {
    0x1EAF9417u,  // GUIDE
    0xAAA3DC58u,  // GUIDE
    0xACDB130Du,  // GUIDE
    0x1854A8EBu,  // XBOX GUIDE
    0x1A3A3D5Du,  // XBOX GUIDE
    0x75D4240Cu,  // XBOX GUIDE
};

constexpr bool IsGuideLabelKey(std::uint32_t key_hash) noexcept {
  for (std::uint32_t candidate : kGuideLabelKeys) {
    if (candidate == key_hash) {
      return true;
    }
  }
  return false;
}

constexpr bool IsGuideLabelText(std::string_view text) noexcept {
  return text == "GUIDE" || text == "XBOX GUIDE";
}

// Only rewrite a lookup that both uses a known guide key and resolves to the
// stock guide text. An empty result (missing key) or a reused hash with other
// text is left alone so unrelated UI cannot change.
constexpr bool ShouldReplaceGuideLabel(bool enabled, std::uint32_t key_hash,
                                       std::string_view resolved) noexcept {
  return enabled && IsGuideLabelKey(key_hash) && IsGuideLabelText(resolved);
}

}  // namespace gta4::frontend_labels
