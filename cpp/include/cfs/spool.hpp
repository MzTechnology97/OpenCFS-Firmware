// SPDX-License-Identifier: Apache-2.0
// Remaining-filament estimate from the filament length per spool revolution
// (docs/05-preload-rfid.md section 5).
#pragma once

#include <cstdint>

#include "cfs/config.hpp"

namespace cfs {

struct SpoolEstimate {
    uint8_t linear_pct = 0xFF;  // diameter-based
    uint8_t area_pct = 0xFF;    // winding-area based (reported value)
    bool reliable = false;      // false when clamped to the core circumference
};

// `per_rev_mm` = odometer distance between two passes of the spool tag.
SpoolEstimate estimate_remaining(float per_rev_mm, const SpoolGeometry& g);

}  // namespace cfs
