#pragma once
#include <cstdint>

namespace aurora {
// VI supplies the physical display period. Only a sealed frame with an actual
// midpoint owns a two-period span; selecting 60 FPS alone must not delay 2D.
constexpr uint64_t presentation_span(uint64_t viInterval, bool thirtyToSixty) {
  return viInterval * (thirtyToSixty ? 2u : 1u);
}

constexpr uint64_t presentation_deadline(uint64_t base, uint64_t span,
                                         uint32_t insertedFrames, uint32_t slot) {
  if (base == 0 || span == 0) return 0;
  if (insertedFrames == 0) return base - span + 6'500'000;
  return base + span * slot / (insertedFrames + 1u);
}
} // namespace aurora
