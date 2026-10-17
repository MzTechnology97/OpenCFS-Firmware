// SPDX-License-Identifier: Apache-2.0
// Vector table, reset entry, fault handling and C++ runtime support of the
// OpenCFS image. The Creality loader validates the image, loads MSP from
// 0x08003000 and branches to the reset vector at 0x08003004 with every NVIC line
// disabled (docs/08-loader-and-recovery.md section 1).
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "board_io.hpp"
#include "gd32f30x_regs.hpp"
#include "image.hpp"

using Handler = void (*)();

// Linker script symbols (gd32f303xe_app.ld).
extern "C" {
extern const uint32_t _estack;
extern const uint32_t _sidata;
extern uint32_t _sdata, _edata, _sbss, _ebss;
extern const Handler __init_array_start[], __init_array_end[];

void Reset_Handler();
void Fault_Handler();
void SysTick_Handler();
void USART1_IRQHandler();
void EXTI10_15_IRQHandler();
[[noreturn]] void opencfs_reset();
[[noreturn]] void opencfs_fault();
}

namespace cfs::image {
void main_loop();  // main.cpp
}

namespace {

// GD32F30x: 16 system entries and up to 68 interrupts (connectivity line). The
// table is 0x150 bytes, below the 12-byte version string the loader reads at +0x200.
constexpr int kIrqCount = 68;

struct VectorTable {
    const void* initial_sp;
    Handler handlers[15 + kIrqCount];  // handlers[k] is exception k + 1
};

constexpr VectorTable make_vectors() {
    VectorTable t{&_estack, {}};
    for (Handler& h : t.handlers) h = Fault_Handler;  // anything unexpected stops the motors
    t.handlers[0] = Reset_Handler;
    for (int reserved : {6, 7, 8, 9, 12}) t.handlers[reserved] = nullptr;
    t.handlers[14] = SysTick_Handler;
    t.handlers[15 + cfs::gd32::IRQ_USART1] = USART1_IRQHandler;
    t.handlers[15 + cfs::gd32::IRQ_EXTI10_15] = EXTI10_15_IRQHandler;
    return t;
}

[[gnu::section(".isr_vector"), gnu::used]] constexpr VectorTable kVectors = make_vectors();
static_assert(sizeof(VectorTable) == 0x150, "vector table must stay below the +0x200 header");

// Creality image header at +0x200: 12-byte version, CRC-16 at +0x20C, length at
// +0x20E. Left erased here; the packager writes it after linking.
[[gnu::section(".cfs_header"), gnu::used]] const uint8_t kCfsHeader[20] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

}  // namespace

// The FPU must be on before any compiled code runs: the image uses the
// hard-float ABI. Nothing else happens before opencfs_reset().
extern "C" [[gnu::naked]] void Reset_Handler() {
    asm volatile(
        "ldr r0, =0xE000ED88\n"  // SCB CPACR
        "ldr r1, [r0]\n"
        "orr r1, r1, #0x00F00000\n"  // CP10, CP11 full access
        "str r1, [r0]\n"
        "dsb\n"
        "isb\n"
        "b opencfs_reset\n"
        ".ltorg\n");
}

// Every fault and unexpected interrupt: take a fresh stack first, so a stack
// overflow still reaches the safe state.
extern "C" [[gnu::naked]] void Fault_Handler() {
    asm volatile(
        "cpsid i\n"
        "ldr r0, =_estack\n"
        "msr msp, r0\n"
        "b opencfs_fault\n"
        ".ltorg\n");
}

#if CFS_IMAGE_VARIANT == 5
// BUS-LED: stage n lights slot n's red LED (PC6..PC9, open-collector drivers,
// high = on). Register access only, so it works before .data/.bss.
static void stage_led(int n) {
    using namespace cfs::gd32;
    using namespace cfs::board;
    reg(RCU_APB2EN) |= APB2_PC;
    pin_write(pins::kLedB[n], true);
    pin_mode(pins::kLedB[n], PinMode::OutputPP);
}
#define STAGE(n) stage_led(n)
#else
#define STAGE(n) ((void)0)
#endif

extern "C" void opencfs_reset() {
    using namespace cfs::gd32;
    asm volatile("cpsid i" ::: "memory");
    STAGE(0);  // the image runs
    // First of all, free the RS-485 bus. This board has no direction pin: the
    // transceiver follows TX (PA2), and anything but a high PA2 holds the bus
    // for every device on it, steppers included (docs/10 section 2). The loader
    // reset GPIOA before the jump, so PA2 floats until it is driven here.
    cfs::image::release_bus();
    STAGE(1);  // PA2 held high
    for (uint32_t i = 0; i < 3; ++i) {  // the loader cleared them; do not rely on it
        reg(NVIC_ICER + 4 * i) = 0xFFFFFFFFu;
        reg(NVIC_ICPR + 4 * i) = 0xFFFFFFFFu;
    }
    reg(SCB_VTOR) = cfs::image::kAppBase;
    asm volatile("dsb\nisb" ::: "memory");

    cfs::image::clock_init();
    STAGE(2);  // 120 MHz clock up

    const uint32_t* src = &_sidata;
    for (uint32_t* dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (uint32_t* dst = &_sbss; dst < &_ebss;) *dst++ = 0;
    for (const Handler* ctor = __init_array_start; ctor < __init_array_end; ++ctor) (*ctor)();

    asm volatile("cpsie i" ::: "memory");
    cfs::image::main_loop();
    cfs::image::fault_safe_state();  // main_loop() does not return
}

extern "C" void opencfs_fault() { cfs::image::fault_safe_state(); }

namespace cfs::image {

// PA2 as a plain output held high: the bus idles, whatever the USART does.
// Uses no RAM, so it can run before .data/.bss and from the fault handler.
void release_bus() {
    using namespace gd32;
    using namespace board;
    reg(RCU_APB2EN) |= APB2_PA;
    pin_write(pins::kRs485Tx, true);  // output level first, then the mode
    pin_mode(pins::kRs485Tx, PinMode::OutputPP);
}

void fault_safe_state() {
    using namespace gd32;
    using namespace board;
    asm volatile("cpsid i" ::: "memory");
    release_bus();  // a stopped box must not hold the bus
#if CFS_IMAGE_VARIANT >= 4
    // BUS-RELEASE and BUS-LED never configure a motor pin; a fault must not either.
#if CFS_IMAGE_VARIANT == 5
    reg(RCU_APB2EN) |= APB2_PB;
    pin_write(pins::kLedA[0], true);  // slot 1 white LED: a fault was taken
    pin_mode(pins::kLedA[0], PinMode::OutputPP);
#endif
    for (;;) {
    }
#endif
    reg(RCU_APB2EN) |= APB2_PD | APB2_PE;  // no effect if the board already did it
    for (int s = 0; s < 4; ++s) {          // AT8236 IN1 = IN2 = high: brake
        pin_write(pins::kFeederIn1[s], true);
        pin_write(pins::kFeederIn2[s], true);
    }
    pin_write(pins::kHubBrake, true);
    // The hub PWM is active low: hold the pin high as a plain output, so TIMER0
    // no longer drives it whatever its state.
    pin_write(pins::kHubPwm, true);
    pin_mode(pins::kHubPwm, PinMode::OutputPP);
    for (;;) {
    }
}

}  // namespace cfs::image

// ---- C++ runtime support: no heap, no exceptions, nothing to destroy at exit ----
extern "C" void __cxa_pure_virtual() { cfs::image::fault_safe_state(); }
void operator delete(void*) noexcept { cfs::image::fault_safe_state(); }
void operator delete(void*, std::size_t) noexcept { cfs::image::fault_safe_state(); }
