// SPDX-License-Identifier: Apache-2.0
// Entry of the OpenCFS image: one BoardApp, the cooperative loop, and the three
// interrupt handlers it needs (docs/13-application.md sections 4 and 5).
//
// The build variant is fixed at compile time (tools/build_image.py --variant):
//   CFS_IMAGE_VARIANT 1  RELEASE  the normal firmware
//   CFS_IMAGE_VARIANT 2  INERT    boot and RS-485 only: no output can be driven,
//                                 the parameter page is never written
//   CFS_IMAGE_VARIANT 3  BENCH    INERT plus the 0x7B test commands (bench.hpp)
#include <cstdint>
#include <new>

#include "board_app.hpp"
#include "cfs/build_info.hpp"
#include "image.hpp"
#include "vendor_decoders.hpp"

#ifndef CFS_IMAGE_VARIANT
#error "CFS_IMAGE_VARIANT must be 1 (RELEASE), 2 (INERT) or 3 (BENCH)"
#endif
#ifndef CFS_BUILD_REVISION
#define CFS_BUILD_REVISION "unknown"
#endif

#if CFS_IMAGE_VARIANT == 3
#include "bench.hpp"
#endif

namespace {

constexpr uint8_t kVariant = CFS_IMAGE_VARIANT;
static_assert(kVariant >= cfs::BuildInfo::Release && kVariant <= cfs::BuildInfo::Bench, "bad CFS_IMAGE_VARIANT");
constexpr bool kLocked = kVariant != cfs::BuildInfo::Release;
constexpr uint8_t kFlags = (kLocked ? cfs::BuildInfo::MotionLocked | cfs::BuildInfo::SettingsReadOnly : 0) |
                           (CFS_HAVE_CREALITY_KEYS || CFS_HAVE_VENDOR_KEYS ? cfs::BuildInfo::VendorKeys : 0);

constexpr const char* variant_name() {
    return kVariant == cfs::BuildInfo::Inert ? "OpenCFS-INERT" :
           kVariant == cfs::BuildInfo::Bench ? "OpenCFS-BENCH" : "OpenCFS-RELEASE";
}

// Identity at the fixed offset +0x220 of the image, so a host tool can tell the
// variant from the .bin file itself (docs/13 section 5). Same values as 0x7A.
struct ImageInfo {
    char magic[8];
    uint8_t format, variant, flags, reserved;
    char revision[12];
    char name[16];
};
static_assert(sizeof(ImageInfo) == 40, "image info layout");

constexpr ImageInfo make_info() {
    ImageInfo i{{'O', 'C', 'F', 'S', 'I', 'N', 'F', 'O'}, 1, kVariant, kFlags, 0, {}, {}};
    const char* r = CFS_BUILD_REVISION;
    for (int k = 0; k < 12 && r[k]; ++k) i.revision[k] = r[k];
    const char* n = variant_name();
    for (int k = 0; k < 16 && n[k]; ++k) i.name[k] = n[k];
    return i;
}

[[gnu::section(".ocfs_info"), gnu::used]] constexpr ImageInfo kImageInfo = make_info();

// Constructed in main_loop() rather than as globals, so nothing runs before the
// clock is up and no destructor is ever registered.
alignas(cfs::board::BoardApp) uint8_t g_storage[sizeof(cfs::board::BoardApp)];
cfs::board::BoardApp* g_app = nullptr;
cfs::BuildInfo g_build;

#if CFS_IMAGE_VARIANT == 3
alignas(cfs::board::BenchCommands) uint8_t g_bench_storage[sizeof(cfs::board::BenchCommands)];

[[noreturn]] void raise_fault() {
    asm volatile("udf #0");  // a real UsageFault, taken by Fault_Handler
    for (;;) {
    }
}
#endif

// Unassigned address: auto-addressing from the host assigns one, as with stock.
constexpr uint8_t kUnassignedAddress = 0xFE;

}  // namespace

extern "C" void SysTick_Handler() {
    if (g_app) g_app->on_systick();
}

extern "C" void USART1_IRQHandler() {
    if (g_app) g_app->on_usart1_irq();
}

extern "C" void EXTI10_15_IRQHandler() {
    if (g_app) g_app->on_exti10_15_irq();
}

namespace cfs::image {

void main_loop() {
    board::BoardVariant v;
    v.motion_locked = kLocked;
    v.settings_read_only = kLocked;
    g_app = new (g_storage) board::BoardApp(kUnassignedAddress, v);

    g_build.variant = kVariant;
    g_build.flags = kFlags;
    for (int k = 0; k < 12; ++k) g_build.revision[k] = kImageInfo.revision[k];
    const volatile char* id = reinterpret_cast<const volatile char*>(static_cast<uintptr_t>(kAppBase + 0x200));
    for (int k = 0; k < 12; ++k) g_build.app_id[k] = id[k];  // written by the packager
#if CFS_IMAGE_VARIANT == 3
    auto* bench = new (g_bench_storage) board::BenchCommands(*g_app, &raise_fault);
    g_build.bench_level = bench->level_ptr();
    g_app->app().disp.set_extension(bench);
    g_app->set_service(bench);
#endif
    g_app->app().disp.set_build_info(&g_build);

    g_app->init();
    for (;;) g_app->loop_once();
}

}  // namespace cfs::image
