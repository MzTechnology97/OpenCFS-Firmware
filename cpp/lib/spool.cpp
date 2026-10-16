// SPDX-License-Identifier: Apache-2.0
#include "cfs/spool.hpp"

namespace cfs {

namespace {

// The stock firmware uses pi = 3.14 in these formulas; keep it for identical results.
constexpr float kPi = 3.14f;

uint8_t clamp_pct(float v) {
    const float r = v * 100.0f + 0.5f;
    if (r < 1.0f) return 1;
    if (r >= 100.0f) return 100;
    return static_cast<uint8_t>(r);
}

}  // namespace

SpoolEstimate estimate_remaining(float per_rev_mm, const SpoolGeometry& g) {
    SpoolEstimate e;
    if (per_rev_mm < 0.0f) {
        return e;
    }
    const float core_circ = kPi * g.core_diameter_mm;
    const float full_circ = kPi * g.full_diameter_mm;
    e.reliable = per_rev_mm >= core_circ;
    const float circ = e.reliable ? per_rev_mm : core_circ;

    e.linear_pct = clamp_pct((circ - core_circ) / (full_circ - core_circ));

    const float r = circ / (2.0f * kPi);
    const float rc = g.core_diameter_mm / 2.0f;
    const float rf = g.full_diameter_mm / 2.0f;
    e.area_pct = clamp_pct((kPi * r * r - kPi * rc * rc) / (kPi * (rf * rf - rc * rc)));
    return e;
}

}  // namespace cfs
