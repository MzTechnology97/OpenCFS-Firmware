// SPDX-License-Identifier: Apache-2.0
// Build identity reported by command 0x7A (docs/13-application.md §5), so a host
// can never mistake a test build for the release one.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfs {

struct BuildInfo {
    enum Variant : uint8_t { Release = 1, Inert = 2, Bench = 3 };
    enum Flags : uint8_t { MotionLocked = 1u << 0, SettingsReadOnly = 1u << 1, VendorKeys = 1u << 2 };
    enum BenchLevel : uint8_t { NotBench = 0, SensorsOnly = 1, ActuatorTest = 2 };

    uint8_t variant = Release;
    uint8_t flags = 0;
    char revision[12] = {};  // source revision, NUL padded
    char app_id[12] = {};    // the 12 bytes at image +0x200 (Creality version)
    // Current bench level, read at every reply; nullptr outside BENCH builds.
    const volatile uint8_t* bench_level = nullptr;
};

// 0x7A reply: "OCFS", format 1, variant, bench level, flags, revision[12], app_id[12].
constexpr size_t kBuildInfoLen = 32;

inline size_t encode_build_info(const BuildInfo& b, uint8_t out[kBuildInfoLen]) {
    out[0] = 'O';
    out[1] = 'C';
    out[2] = 'F';
    out[3] = 'S';
    out[4] = 1;
    out[5] = b.variant;
    out[6] = b.bench_level ? *b.bench_level : static_cast<uint8_t>(BuildInfo::NotBench);
    out[7] = b.flags;
    for (size_t i = 0; i < 12; ++i) out[8 + i] = static_cast<uint8_t>(b.revision[i]);
    for (size_t i = 0; i < 12; ++i) out[20 + i] = static_cast<uint8_t>(b.app_id[i]);
    return kBuildInfoLen;
}

}  // namespace cfs
