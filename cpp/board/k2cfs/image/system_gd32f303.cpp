// SPDX-License-Identifier: Apache-2.0
// Clock set-up of the OpenCFS image (docs/13-application.md section 2): HXTAL
// 8 MHz / 2 x 30 = 120 MHz SYSCLK, AHB and APB2 /1, APB1 /2. The loader has
// already run its own PLL set-up, so the clock tree is first put back on IRC8M
// before the PLL is reprogrammed, as the stock application does.
#include "image.hpp"

#include "gd32f30x_regs.hpp"

namespace cfs::image {

using namespace gd32;

namespace {

constexpr uint32_t kHxtalTimeout = 0xFFFF;

void spin(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i = i + 1) {
    }
}

// Back to the reset clock tree: SYSCLK from IRC8M, PLL and HXTAL off.
void clock_reset() {
    reg(RCU_CTL) |= RCU_CTL_IRC8MEN;
    reg(RCU_CFG0) = (reg(RCU_CFG0) & ~RCU_CFG0_AHBPSC_MASK) | (8u << 4);  // AHB /2 while switching
    spin(20000);
    reg(RCU_CFG0) = 0;  // SCS = IRC8M, all prescalers /1, PLL bits cleared
    for (uint32_t i = 0; i < kHxtalTimeout && (reg(RCU_CFG0) & RCU_CFG0_SCSS_MASK) != 0; ++i) {
    }
    reg(RCU_CFG1) = 0;
    reg(RCU_CTL) &= ~(RCU_CTL_HXTALEN | RCU_CTL_CKMEN | RCU_CTL_PLLEN);
    reg(RCU_INT) = RCU_INT_CLEAR_ALL;
    reg(RCU_CTL) &= ~RCU_CTL_HXTALBPS;
}

}  // namespace

void clock_init() {
    clock_reset();

    reg(RCU_CTL) |= RCU_CTL_HXTALEN;
    uint32_t n = 0;
    while (!(reg(RCU_CTL) & RCU_CTL_HXTALSTB) && n < kHxtalTimeout) ++n;
    if (!(reg(RCU_CTL) & RCU_CTL_HXTALSTB)) {
        // No crystal: every baud rate and timer would be wrong. Stock stops here
        // too; the motor outputs are still floating inputs at this point.
        fault_safe_state();
    }

    reg(RCU_APB1EN) |= APB1_PMU;
    reg(PMU_CTL) |= PMU_CTL_LDOVS_HIGH;

    uint32_t cfg = reg(RCU_CFG0);
    cfg &= ~(RCU_CFG0_AHBPSC_MASK | (7u << 8) | (7u << 11));  // AHB /1, APB1 and APB2 cleared
    cfg |= RCU_CFG0_APB1PSC_DIV2;                             // APB1 /2 = 60 MHz, APB2 /1
    cfg |= RCU_CFG0_PLLSEL_HXTAL | RCU_CFG0_PREDV0_DIV2;      // PLL input HXTAL / 2 = 4 MHz
    cfg = (cfg & ~RCU_CFG0_PLLMF_MASK) | RCU_CFG0_PLLMF_MUL30;
    reg(RCU_CFG0) = cfg;

    reg(RCU_CTL) |= RCU_CTL_PLLEN;
    while (!(reg(RCU_CTL) & RCU_CTL_PLLSTB)) {
    }

    // High-drive mode, required above 108 MHz.
    reg(PMU_CTL) |= PMU_CTL_HDEN;
    while (!(reg(PMU_CS) & PMU_CS_HDRF)) {
    }
    reg(PMU_CTL) |= PMU_CTL_HDS;
    while (!(reg(PMU_CS) & PMU_CS_HDSRF)) {
    }

    reg(RCU_CFG0) = (reg(RCU_CFG0) & ~RCU_CFG0_SCS_MASK) | RCU_CFG0_SCS_PLL;
    while ((reg(RCU_CFG0) & RCU_CFG0_SCSS_MASK) != RCU_CFG0_SCSS_PLL) {
    }
}

}  // namespace cfs::image
