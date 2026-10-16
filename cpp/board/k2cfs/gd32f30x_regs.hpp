// SPDX-License-Identifier: Apache-2.0
// Minimal GD32F30x register map used by the K2 CFS board port. Written from the
// public GD32F30x user manual register descriptions (addresses and bit positions
// are hardware facts); no vendor library code is included.
#pragma once

#include <cstdint>

namespace cfs::gd32 {

inline volatile uint32_t& reg(uint32_t addr) {
    return *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(addr));
}

// ---- base addresses -------------------------------------------------------
constexpr uint32_t AFIO = 0x40010000, EXTI = 0x40010400;
constexpr uint32_t GPIOA = 0x40010800, GPIOB = 0x40010C00, GPIOC = 0x40011000,
                   GPIOD = 0x40011400, GPIOE = 0x40011800;
constexpr uint32_t ADC0 = 0x40012400, ADC1 = 0x40012800, TIMER0 = 0x40012C00;
constexpr uint32_t SPI1 = 0x40003800, USART1 = 0x40004400;
constexpr uint32_t RCU = 0x40021000;
constexpr uint32_t SYSTICK = 0xE000E010, NVIC_ISER = 0xE000E100;

// ---- RCU --------------------------------------------------------------------
constexpr uint32_t RCU_CTL = RCU + 0x00, RCU_CFG0 = RCU + 0x04, RCU_INT = RCU + 0x08,
                   RCU_APB2EN = RCU + 0x18, RCU_APB1EN = RCU + 0x1C, RCU_CFG1 = RCU + 0x2C;
enum : uint32_t {
    RCU_CTL_IRC8MEN = 1u << 0, RCU_CTL_HXTALEN = 1u << 16, RCU_CTL_HXTALSTB = 1u << 17,
    RCU_CTL_HXTALBPS = 1u << 18, RCU_CTL_CKMEN = 1u << 19, RCU_CTL_PLLEN = 1u << 24,
    RCU_CTL_PLLSTB = 1u << 25,
    RCU_CFG0_SCS_MASK = 3u << 0, RCU_CFG0_SCS_PLL = 2u << 0, RCU_CFG0_SCSS_MASK = 3u << 2,
    RCU_CFG0_SCSS_PLL = 2u << 2, RCU_CFG0_AHBPSC_MASK = 0xFu << 4, RCU_CFG0_APB1PSC_DIV2 = 4u << 8,
    RCU_CFG0_PLLSEL_HXTAL = 1u << 16, RCU_CFG0_PREDV0_DIV2 = 1u << 17,
    // PLLMF is bits 21:18 plus bit 27 (bit 4) and bit 30 (bit 5); x30 is 0b011101.
    RCU_CFG0_PLLMF_MASK = (0xFu << 18) | (1u << 27) | (1u << 30),
    RCU_CFG0_PLLMF_MUL30 = (0xDu << 18) | (1u << 27),
    RCU_INT_CLEAR_ALL = 0x009F0000,  // IRC40K..CKM ready/fail flag clears
    APB1_PMU = 1u << 28,
};
constexpr uint32_t PMU = 0x40007000, PMU_CTL = PMU + 0x00, PMU_CS = PMU + 0x04;
enum : uint32_t {
    PMU_CTL_LDOVS_HIGH = 3u << 14, PMU_CTL_HDEN = 1u << 16, PMU_CTL_HDS = 1u << 17,
    PMU_CS_HDRF = 1u << 16, PMU_CS_HDSRF = 1u << 17,
};
constexpr uint32_t RCU_CFG0_ADCPSC_MASK = (3u << 14) | (1u << 28), RCU_CFG0_ADCPSC_DIV6 = 2u << 14;
enum : uint32_t {
    APB2_AF = 1u << 0, APB2_PA = 1u << 2, APB2_PB = 1u << 3, APB2_PC = 1u << 4,
    APB2_PD = 1u << 5, APB2_PE = 1u << 6, APB2_ADC0 = 1u << 9, APB2_ADC1 = 1u << 10, APB2_TIMER0 = 1u << 11,
    APB1_SPI1 = 1u << 14, APB1_USART1 = 1u << 17,
};

// ---- GPIO (CTL0 pins 0..7, CTL1 pins 8..15, 4 bits per pin) ---------------
constexpr uint32_t GPIO_CTL0 = 0x00, GPIO_CTL1 = 0x04, GPIO_ISTAT = 0x08, GPIO_OCTL = 0x0C,
                   GPIO_BOP = 0x10, GPIO_BC = 0x14;
enum class PinMode : uint32_t {
    Analog = 0x0,      // CTL=00 MD=00
    InputFloat = 0x4,  // CTL=01 MD=00
    InputPull = 0x8,   // CTL=10 MD=00, pull direction from OCTL
    OutputPP = 0x3,    // CTL=00 MD=11 (50 MHz)
    OutputOD = 0x7,    // CTL=01 MD=11
    AltPP = 0xB,       // CTL=10 MD=11
};
constexpr uint32_t AFIO_PCF0 = AFIO + 0x04;
constexpr uint32_t AFIO_EXTISS0 = AFIO + 0x08;  // EXTISS0..3, 4 bits per line
constexpr uint32_t PCF0_TIMER0_FULL_REMAP = 3u << 6;

// ---- EXTI ------------------------------------------------------------------
constexpr uint32_t EXTI_INTEN = EXTI + 0x00, EXTI_RTEN = EXTI + 0x08, EXTI_FTEN = EXTI + 0x0C,
                   EXTI_PD = EXTI + 0x14;

// ---- TIMER -----------------------------------------------------------------
constexpr uint32_t TIM_CTL0 = 0x00, TIM_SWEVG = 0x14, TIM_CHCTL0 = 0x18, TIM_CHCTL1 = 0x1C,
                   TIM_CHCTL2 = 0x20, TIM_PSC = 0x28, TIM_CAR = 0x2C, TIM_CH0CV = 0x34,
                   TIM_CCHP = 0x44;
constexpr uint32_t TIM_CTL0_CEN = 1u << 0, TIM_CTL0_ARSE = 1u << 7;
constexpr uint32_t TIM_CCHP_POEN = 1u << 15;  // advanced timers (TIMER0) only

// ---- USART -----------------------------------------------------------------
constexpr uint32_t USART_STAT0 = 0x00, USART_DATA = 0x04, USART_BAUD = 0x08, USART_CTL0 = 0x0C;
enum : uint32_t {
    USART_STAT0_ERRORS = 0xFu,  // PERR, FERR, NERR, ORERR
    USART_STAT0_RBNE = 1u << 5, USART_STAT0_TC = 1u << 6, USART_STAT0_TBE = 1u << 7,
    USART_CTL0_REN = 1u << 2, USART_CTL0_TEN = 1u << 3, USART_CTL0_RBNEIE = 1u << 5,
    USART_CTL0_UEN = 1u << 13,
};

// ---- SPI -------------------------------------------------------------------
constexpr uint32_t SPI_CTL0 = 0x00, SPI_STAT = 0x08, SPI_DATA = 0x0C;
enum : uint32_t {
    SPI_CTL0_CKPH = 1u << 0, SPI_CTL0_CKPL = 1u << 1, SPI_CTL0_MSTMOD = 1u << 2,
    SPI_CTL0_SPIEN = 1u << 6, SPI_CTL0_SWNSS = 1u << 8, SPI_CTL0_SWNSSEN = 1u << 9,
    SPI_STAT_RBNE = 1u << 0, SPI_STAT_TBE = 1u << 1, SPI_STAT_TRANS = 1u << 7,
};

// ---- ADC -------------------------------------------------------------------
constexpr uint32_t ADC_STAT = 0x00, ADC_CTL1 = 0x08, ADC_SAMPT0 = 0x0C, ADC_SAMPT1 = 0x10,
                   ADC_RSQ0 = 0x2C, ADC_RSQ2 = 0x34, ADC_RDATA = 0x4C;
enum : uint32_t {
    ADC_STAT_EOC = 1u << 1, ADC_CTL1_ADCON = 1u << 0, ADC_CTL1_CLB = 1u << 2,
    ADC_CTL1_RSTCLB = 1u << 3, ADC_CTL1_ETSRC_SW = 7u << 17, ADC_CTL1_ETERC = 1u << 20,
    ADC_CTL1_SWRCST = 1u << 22,
};

// ---- FMC (flash controller, bank 0 covers the whole 512 KiB part) ------------
constexpr uint32_t FMC = 0x40022000;
constexpr uint32_t FMC_KEY0 = FMC + 0x04, FMC_STAT0 = FMC + 0x0C, FMC_CTL0 = FMC + 0x10,
                   FMC_ADDR0 = FMC + 0x14;
constexpr uint32_t FMC_UNLOCK_KEY0 = 0x45670123, FMC_UNLOCK_KEY1 = 0xCDEF89AB;
enum : uint32_t {
    FMC_STAT_BUSY = 1u << 0, FMC_STAT_PGERR = 1u << 2, FMC_STAT_WPERR = 1u << 4,
    FMC_STAT_ENDF = 1u << 5,
    FMC_CTL_PG = 1u << 0, FMC_CTL_PER = 1u << 1, FMC_CTL_START = 1u << 6, FMC_CTL_LK = 1u << 7,
};

// ---- SysTick / NVIC ------------------------------------------------------------
constexpr uint32_t SYST_CTRL = SYSTICK + 0x0, SYST_LOAD = SYSTICK + 0x4, SYST_VAL = SYSTICK + 0x8;
constexpr uint32_t SCB_VTOR = 0xE000ED08, SCB_CPACR = 0xE000ED88;
constexpr uint32_t NVIC_ICER = 0xE000E180, NVIC_ICPR = 0xE000E280;

inline void nvic_enable(uint32_t irq) { reg(NVIC_ISER + 4 * (irq / 32)) = 1u << (irq % 32); }

// IRQ numbers (GD32F30x high density).
constexpr uint32_t IRQ_EXTI5_9 = 23, IRQ_USART1 = 38, IRQ_EXTI10_15 = 40;

}  // namespace cfs::gd32
