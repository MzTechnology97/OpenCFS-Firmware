#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot the linked OpenCFS ARM image in an emulator (Unicorn, Cortex-M4) with a
minimal GD32F303 peripheral model, and talk to it over a simulated RS-485 link.

It runs the real binary from the reset vector, as the Creality loader would,
and checks:
- start-up: no fault handler reached, VTOR at 0x08003000, the stock clock tree
  (CFG0), a 1 ms SysTick, feeders and hub left braked;
- flash: the image erases and programs only the parameter page 0x0807F800;
- protocol: auto-addressing (A1 discover, A0 assign, three replies) and a box
  command at the new address, with valid CRC-8 on every reply frame.

This is a model, not hardware: peripheral timing, analog inputs, the RFID
readers and the motor drivers are not represented. Needs `pip install unicorn`.

usage: image_boot_test.py IMAGE.bin    (sealed or unsealed, linked at 0x08003000)
"""
import struct
import sys

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_MCLASS, \
    UC_MODE_THUMB, Uc, UcError
from unicorn import arm_const as A

APP, FLASH, FLASH_SIZE, PARAM = 0x08003000, 0x08000000, 0x80000, 0x0807F800
SRAM, SYSMEM, PERIPH, PPB = 0x20000000, 0x1FFFF000, 0x40000000, 0xE0000000
RAM_TOP = SRAM + 0x10000
UID = bytes(range(0x30, 0x3C))
HCLK = 120_000_000
RETURN_MAGIC = 0x08001000  # inside the loader area, never executed by the image

RCU, PMU, FMC, USART1, SYST = 0x40021000, 0x40007000, 0x40022000, 0x40004400, 0xE000E010
GPIO = {p: 0x40010800 + 0x400 * i for i, p in enumerate("ABCDE")}
ADC0, ADC1, SPI1 = 0x40012400, 0x40012800, 0x40003800
IRQ_USART1 = 38


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def frame(addr, hdr, cmd, payload=b""):
    body = bytes([len(payload) + 3, hdr, cmd]) + payload
    return bytes([0xF7, addr]) + body + bytes([crc8(body)])


class Board:
    def __init__(self, image):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_M4)
        for base, size in ((FLASH, FLASH_SIZE), (SYSMEM, 0x1000), (SRAM, 0x10000),
                           (PERIPH, 0x30000), (PPB, 0x100000)):
            uc.mem_map(base, size)
        uc.mem_write(FLASH, b"\xff" * FLASH_SIZE)
        uc.mem_write(APP, image)
        uc.mem_write(0x1FFFF7E8, UID)
        self.image = image
        self.cycles = 0
        self.reads_in_chunk = 0
        self.rx, self.tx = bytearray(), bytearray()
        self.flash_writes, self.erases, self.problems = [], [], []
        self.faults = 0
        vec = struct.unpack_from("<84I", image, 0)
        self.sp0, self.reset = vec[0], vec[1]
        self.systick_handler, self.usart_handler, self.fault = vec[15], vec[16 + IRQ_USART1], vec[3]
        # Hooks only on the modelled registers; everything else is plain memory.
        modelled = [(RCU, 8), (PMU + 4, 4), (FMC + 0x0C, 4), (SYST + 8, 4), (USART1, 8), (SPI1 + 8, 4)]
        modelled += [(adc, 4) for adc in (ADC0, ADC1)] + [(adc + 8, 4) for adc in (ADC0, ADC1)]
        modelled += [(g + 8, 4) for g in GPIO.values()]
        for base, size in modelled:
            uc.hook_add(UC_HOOK_MEM_READ, self._read, begin=base, end=base + size - 1)
        for base in [USART1 + 4, FMC + 0x10] + [g + off for g in GPIO.values() for off in (0x10, 0x14)]:
            uc.hook_add(UC_HOOK_MEM_WRITE, self._write_periph, begin=base, end=base + 3)
        self.nvic_enabled = [0, 0, 0]  # ISER is write-one-to-set, ICER write-one-to-clear
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write_nvic, begin=0xE000E100, end=0xE000E18B)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write_flash, begin=FLASH, end=FLASH + FLASH_SIZE - 1)
        uc.hook_add(UC_HOOK_CODE, self._fault_hit, begin=self.fault & ~1, end=self.fault & ~1)

    # ---- memory helpers ----
    def r32(self, addr):
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def w32(self, addr, value):
        self.uc.mem_write(addr, struct.pack("<I", value & 0xFFFFFFFF))

    # ---- peripheral model ----
    def _read(self, uc, _access, addr, size, _value, _data):
        a = addr & ~3
        if a == RCU:  # CTL: ready flags follow their enables
            v = self.r32(a) | 0x2
            if v & (1 << 16): v |= 1 << 17
            if v & (1 << 24): v |= 1 << 25
            self.w32(a, v)
        elif a == RCU + 4:  # CFG0: SCSS follows SCS
            v = self.r32(a)
            self.w32(a, (v & ~0xC) | ((v & 3) << 2))
        elif a == PMU + 4:
            ctl = self.r32(PMU)
            self.w32(a, (0x10000 if ctl & (1 << 16) else 0) | (0x20000 if ctl & (1 << 17) else 0))
        elif a == FMC + 0x0C:
            self.w32(a, 0)  # never busy, no error (the flags are write-one-to-clear)
        elif a == SYST + 8:  # VAL counts down at HCLK
            load = self.r32(SYST + 4) + 1
            self.reads_in_chunk += 1
            self.w32(a, (load - 1) - ((self.cycles + 8 * self.reads_in_chunk) % load))
        elif a == USART1:
            self.w32(a, 0xC0 | (0x20 if self.rx else 0))
        elif a == USART1 + 4 and self.rx:
            self.w32(a, self.rx.pop(0))
        elif a in (ADC0, ADC1):
            self.w32(a, self.r32(a) | 0x2)  # STAT: conversion done
        elif a in (ADC0 + 8, ADC1 + 8):  # CTL1: calibration and software start self-clear
            self.w32(a, self.r32(a) & ~((1 << 2) | (1 << 3) | (1 << 22)))
        elif a == SPI1 + 8:
            self.w32(a, 0x3)
        elif any(a == g + 8 for g in GPIO.values()):
            self.w32(a, 0xFFFF)  # inputs high: no filament, I2C lines released
        return True

    def _write_periph(self, uc, _access, addr, size, value, _data):
        port = next((g for g in GPIO.values() if addr in (g + 0x10, g + 0x14)), None)
        if port is not None:  # BOP sets (low half) and clears (high half); BC clears
            octl = self.r32(port + 0x0C)
            if addr == port + 0x10:
                octl = (octl | (value & 0xFFFF)) & ~(value >> 16)
            else:
                octl &= ~(value & 0xFFFF)
            self.w32(port + 0x0C, octl)
        elif addr == USART1 + 4:
            self.tx.append(value & 0xFF)
        elif addr == FMC + 0x10 and value & (1 << 6) and value & (1 << 1):  # page erase START
            page = self.r32(FMC + 0x14)
            self.erases.append(page)
            if page != PARAM:
                self.problems.append("page erase at 0x%08x" % page)
            else:
                uc.mem_write(PARAM, b"\xff" * 0x800)
        return True

    def _write_nvic(self, uc, _access, addr, size, value, _data):
        if 0xE000E100 <= addr < 0xE000E10C:
            self.nvic_enabled[(addr - 0xE000E100) // 4] |= value
        elif 0xE000E180 <= addr < 0xE000E18C:
            self.nvic_enabled[(addr - 0xE000E180) // 4] &= ~value
        return True

    def _write_flash(self, uc, _access, addr, size, value, _data):
        self.flash_writes.append(addr)
        if not PARAM <= addr < FLASH + FLASH_SIZE:
            self.problems.append("flash write at 0x%08x" % addr)
        if not self.r32(FMC + 0x10) & 1:
            self.problems.append("flash write at 0x%08x without PG" % addr)
        return True

    def _fault_hit(self, uc, address, size, _data):
        self.faults += 1
        if not self.allow_fault:
            uc.emu_stop()

    allow_fault = False

    # ---- execution ----
    def call(self, handler):
        """Run an interrupt handler to completion as if the NVIC had taken it."""
        uc = self.uc
        names = [getattr(A, "UC_ARM_REG_R%d" % i) for i in range(13)] + [
            A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC, A.UC_ARM_REG_XPSR] + [
            getattr(A, "UC_ARM_REG_S%d" % i) for i in range(32)] + [A.UC_ARM_REG_FPSCR]
        saved = [uc.reg_read(n) for n in names]
        uc.reg_write(A.UC_ARM_REG_SP, (saved[13] - 0x40) & ~7)
        uc.reg_write(A.UC_ARM_REG_LR, RETURN_MAGIC | 1)
        uc.emu_start(handler | 1, RETURN_MAGIC, count=200000)
        for n, v in zip(names, saved):
            uc.reg_write(n, v)

    def run_ms(self, ms, chunk=6000):
        uc = self.uc
        end = self.cycles + ms * (HCLK // 1000)
        next_tick = None
        while self.cycles < end and not self.faults:
            pc = uc.reg_read(A.UC_ARM_REG_PC)
            self.reads_in_chunk = 0
            try:
                uc.emu_start(pc | 1, 0xFFFFFFFF, count=chunk)
            except UcError as e:
                self.problems.append("emulation stopped at 0x%08x: %s" % (uc.reg_read(A.UC_ARM_REG_PC), e))
                return
            self.cycles += chunk
            if self.faults:
                return
            primask = uc.reg_read(A.UC_ARM_REG_PRIMASK)
            ctrl, load = self.r32(SYST), self.r32(SYST + 4) + 1
            if ctrl & 3 == 3:
                if next_tick is None:
                    next_tick = self.cycles + load
                while self.cycles >= next_tick and not primask:
                    self.call(self.systick_handler)
                    next_tick += load
            usart_on = self.nvic_enabled[IRQ_USART1 // 32] & (1 << (IRQ_USART1 % 32))
            if self.rx and not primask and usart_on and self.r32(USART1 + 0x0C) & 0x20:
                self.call(self.usart_handler)

    def start(self):
        # As the loader leaves it at the jump (0x08001304..0x0800135E): NVIC cleared,
        # but SysTick still running with its interrupt (HCLK/8, 1 ms at 120 MHz).
        self.w32(SYST + 4, HCLK // 8 // 1000 - 1)
        self.w32(SYST, 0x3)
        self.uc.reg_write(A.UC_ARM_REG_MSP, self.sp0)
        self.uc.reg_write(A.UC_ARM_REG_SP, self.sp0)
        self.uc.reg_write(A.UC_ARM_REG_PC, self.reset)

    def frames(self):
        out, buf = [], bytes(self.tx)
        i = 0
        while i + 6 <= len(buf):
            if buf[i] != 0xF7:
                i += 1
                continue
            n = buf[i + 2]
            f = buf[i:i + 3 + n]
            if len(f) < 3 + n:
                break
            out.append((f, crc8(f[2:-1]) == f[-1]))
            i += 3 + n
        self.tx.clear()
        return out


def main():
    image = open(sys.argv[1], "rb").read()
    b = Board(image)
    b.start()
    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))

    b.run_ms(300)
    check("boot without fault", not b.faults and not b.problems, "; ".join(b.problems))
    check("VTOR = 0x08003000", b.r32(0xE000ED08) == APP, hex(b.r32(0xE000ED08)))
    cfg0 = b.r32(RCU + 4) & ~(0x3 << 14 | 1 << 28 | 0xC)  # ADC prescaler and SCSS aside
    check("clock CFG0 as stock (120 MHz from HXTAL)", cfg0 == 0x08370402, hex(cfg0))
    check("SysTick 1 ms at HCLK", b.r32(SYST + 4) == HCLK // 1000 - 1 and b.r32(SYST) & 7 == 7)
    octl_d = b.r32(GPIO["D"] + 0x0C)
    check("feeders braked (IN1 = IN2 = high)", octl_d & 0xF00F == 0xF00F, hex(octl_d))
    check("hub brake on", b.r32(GPIO["E"] + 0x0C) & (1 << 4))
    check("blank parameter page: erased once, at 0x0807F800", b.erases == [PARAM], str(b.erases))
    check("flash programmed only in the parameter page (4 words)",
          [a - PARAM for a in b.flash_writes] == [0, 4, 8, 12], [hex(a) for a in b.flash_writes])
    check("loader and application area untouched",
          b.uc.mem_read(APP, len(image)) == image and b.uc.mem_read(FLASH, 0x3000) == b"\xff" * 0x3000)
    # Blank page on first boot: defaults written as stock does (docs/10 section 1).
    rec = bytes(b.uc.mem_read(PARAM, 16))
    crc = 0
    for v in rec[8:16]:
        crc ^= v << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    check("parameter record written with a valid CRC, mode 0",
          struct.unpack_from("<II", rec) == (crc, 8) and rec[12] == 0, rec.hex())

    b.rx += frame(0xFE, 0x00, 0xA1, b"\xFE\xFE")
    b.run_ms(1500)
    replies = b.frames()
    disc = [f for f, ok in replies if ok and f[4] == 0xA1]
    check("A1 discover: one reply from 0xFE with the UID", len(disc) == 1 and disc[0][1] == 0xFE
          and disc[0][5:7] == b"\x01\x00" and disc[0][7:19] == UID, [f.hex() for f, _ in replies])

    b.rx += frame(0xFE, 0x00, 0xA0, b"\x01" + UID)
    b.run_ms(500)
    replies = b.frames()
    assign = [f for f, ok in replies if ok and f[4] == 0xA0 and f[1] == 0x01]
    check("A0 assign: three replies from address 1", len(assign) == 3, [f.hex() for f, _ in replies])

    b.rx += bytes.fromhex("f70103ff1406")
    b.run_ms(200)
    replies = b.frames()
    check("command 0x14 answered at address 1 with valid CRC",
          any(ok and f[1] == 0x01 and f[4] == 0x14 for f, ok in replies), [f.hex() for f, _ in replies])
    check("still no fault, no stray flash write", not b.faults and not b.problems, "; ".join(b.problems))

    # A fault while the outputs are driven: feeder 0 forward, hub PWM on TIMER0.
    port_e, port_d = GPIO["E"], GPIO["D"]
    b.w32(port_d + 0x0C, b.r32(port_d + 0x0C) & ~(1 << 3))  # IN1 low: feeder A driving
    b.w32(port_e + 0x0C, b.r32(port_e + 0x0C) & ~(1 << 4))  # hub brake released
    b.allow_fault = True
    b.uc.reg_write(A.UC_ARM_REG_PC, b.fault)
    b.run_ms(1)
    ctl1_e = b.r32(port_e + 0x04)
    check("fault: feeders braked, hub brake on, PWM pin a GPIO held high",
          b.r32(port_d + 0x0C) & 0xF00F == 0xF00F and b.r32(port_e + 0x0C) & (1 << 4)
          and b.r32(port_e + 0x0C) & (1 << 13) and (ctl1_e >> 20) & 0xF == 0x3, hex(ctl1_e))
    check("fault: interrupts masked, MSP reset to the top of SRAM",
          b.uc.reg_read(A.UC_ARM_REG_PRIMASK) == 1 and b.uc.reg_read(A.UC_ARM_REG_MSP) > RAM_TOP - 0x40)

    for name, ok, detail in results:
        print("%-52s %s %s" % (name, "ok  " if ok else "FAIL", "" if ok else detail))
    failed = sum(not ok for _, ok, _ in results)
    print("%d image checks, %d failed, %.2f s emulated" % (len(results), failed, b.cycles / HCLK))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
