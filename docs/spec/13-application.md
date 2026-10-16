# Application loop, clock and watchdog (OpenCFS)

The stock application runs RT-Thread threads and soft timers
(`06-motors-sensors.md` §6). OpenCFS runs the same work in **one cooperative
loop** (`cfs::App`, board glue `cfs::board::BoardApp`).

## 1. Scheduling

| Stock | Period | OpenCFS |
| --- | --- | --- |
| `rs485` thread (dispatcher, long commands inline) | blocking RX | `App::loop_once()` → `Dispatcher::poll()` |
| `preload` thread | 1 ms step | `loop_once()` → `Preload::step()` |
| `print_check` thread | while a refill session is open | `loop_once()` → `PrintTracker::service()` |
| `presd` soft timer | 20 ms | duty: `Preload::sample()` |
| buffer full/empty EXTI | edge | duty: buffer edge → `on_buffer_full/empty()` |
| usage manager | – | duty: `UsageManager::tick()` |
| `motor_blocked_detect_timer` | 100 ms | duty: `MotorSupervisor::tick()` |
| `tim_rotational_speed_detect`, feeder monitors, `mt6826_timer`, `led` | 40 / 20 / 20 ms / – | `Board::tick_1ms()` |
| `lcd` thread (environment) | 1 s | `BoardApp::loop_once()` → `Board::update_environment()` |

Long routines (load, unload, pretension, RFID scan, refill) block `loop_once()`
just as they block their stock thread. While they sleep, the board clock's idle
hook calls `BoardApp::idle()`: it catches up `Board::tick_1ms()` and runs
`App::background()`, which performs the duties above. So supervision,
filtering, the hub speed loop and the buffer edges keep running during every
long routine. Re-entry is guarded: a duty that sleeps (reverse detooth) does not
run the duties again.

**Host traffic during a long routine.** In stock, the `rs485` thread answers
while the `preload` or `print_check` thread works. In OpenCFS, `background()`
polls the dispatcher in a restricted mode:

- answered normally: `0x01` keep-alive, `0x02`, `0x03`, `0x05`, `0x0A`, `0x0C`,
  `0x14`, `0x15`, `0x52`, `0x55`, `0x5B`, `0x57` (its SET/RESET are refused by
  the motion interlock), `0x60`;
- every other command answers **BUSY (`0x03`)**, including those that would start
  motion, reset the odometer (`0x0E`) or change the tracking mode (`0x04`).
  Stock would run some of these concurrently; OpenCFS refuses rather than race.
- While the dispatcher itself is blocked in a command (load stage 4 and the like),
  `background()` does not read the link: the stage's inline reader owns it, as in
  stock (`04-feeder.md` §4).

## 2. Clock (stock application `SystemInit`, `0x08022796`) [code]

- FPU enabled first: `CPACR |= 0xF00000` (`0xE000ED88`). Needed for the
  hard-float ABI.
- HXTAL on, wait for `HXTALSTB` (bounded loop); if it never starts the code
  **hangs in an infinite loop**.
- PMU clock on, LDO high-drive (`PMU_CTL |= 0xC000`), then high-drive enable and
  switch (`HDEN`, `HDS`, waiting for both ready flags).
- `CFG0`: AHB /1, APB2 /1, APB1 /2 (`0x400`), PLL source HXTAL with PREDV0 /2
  (bits 16, 17), PLLMF `0x1D` (bits 21:18 = `0xD`, bit 27) = ×30.
- **8 MHz / 2 × 30 = 120 MHz SYSCLK**, AHB 120 MHz, APB2 120 MHz, APB1 60 MHz
  (timers on APB1 run at 120 MHz). The board port constants (`kHclkHz`,
  `kApb1Hz`, `kTimerClkHz`) match.
- The loader also programs RCU/FMC (literals at `0x0800152C`..`0x08001534`); the
  application sets the clock again regardless.
- VTOR is written by the application (literals `0x08016214`, `0x0801A94C`).

## 3. Watchdog [code]

Neither the application nor the 2025 loader touches the free or window watchdog:
no FWDGT/WWDGT register literal, no `0xCCCC`/`0xAAAA`/`0x5555` key. The only
"WDT" string is RT-Thread's device-class name table. **The stock CFS runs without
a watchdog.** OpenCFS keeps that behavior. Adding FWDGT is a release decision
for the image (feed it in `loop_once()` and in the idle hook, so long routines
keep feeding it), and it must be qualified on hardware like any other change.

## 4. Image (EXPERIMENTAL, not qualified on hardware)

`cpp/board/k2cfs/image/` turns the board port into a linked application image for the
Creality loader. No binary is committed or published; `cpp/tools/build_image.py`
builds one locally.

| File | Content |
| --- | --- |
| `gd32f303xe_app.ld` | Flash `0x08003000..0x0807F7FF`, SRAM 64 KiB, MSP at `0x20010000`. Vector table at `0x08003000` (`0x150` bytes), 18 erased bytes at `+0x200` for the Creality header, link-time checks that the image stays out of the loader and the parameter page |
| `startup.cpp` | Vector table, `Reset_Handler`, fault handling, C++ runtime stubs (no heap, no exceptions, nothing registered for exit) |
| `system_gd32f303.cpp` | The clock set-up of §2 |
| `main.cpp` | One `BoardApp` at address `0xFE`, `init()`, then `loop_once()` forever; SysTick, USART1 and EXTI10-15 handlers |

Start-up order, from the loader's jump (MSP from `0x08003000`, every NVIC line
disabled):

1. FPU on (`CPACR`), before any compiled code runs: the image is hard-float;
2. interrupts masked, NVIC enables and pending bits cleared again, VTOR = `0x08003000`;
3. clock tree back to IRC8M, then the stock 120 MHz set-up of §2. Without HXTAL the
   image stops in the safe state below (stock hangs at the same point);
4. `.data` copied, `.bss` cleared, static constructors run (there are none today);
5. interrupts unmasked, `BoardApp` constructed in static storage, `init()` (1 ms
   SysTick, USART1, EXTI, feeders and hub braked), then the loop.

**Faults.** Every fault and every interrupt without a handler goes to one handler.
It masks interrupts, resets MSP to the top of SRAM so a stack overflow still gets
through, brakes the four feeders (IN1 = IN2 = high) and the hub, and holds the hub
PWM pin high as a plain GPIO output so TIMER0 can no longer drive it. Then it waits
forever. There is no watchdog (§3), so the box stays there until it is power-cycled;
the loader then starts again and keeps its 30 s update window.

**USART1 overrun.** With RBNEIE set, an overrun raises the interrupt without RBNE
and the flag stays until STAT0 then DATA are read. The interrupt handler now does
that read, so a burst on the bus cannot lock the loop out with a stream of
interrupts.

**Known limits.** Some driver loops wait on hardware flags without a timeout
(PLL lock, ADC calibration and conversion, FMC busy). Without a watchdog a stuck
flag stops the box with the outputs as they were. These are the stock behavior
too, and they are on the list for the hardware qualification.

**Image identity.** The packager writes the 12-byte version at `+0x200`, the
CRC-16 and the length (`08-loader-and-recovery.md` §2). The loader accepts any
12 bytes there; `start_app` refuses only an image whose CRC or length is wrong.
The stock updater reads only the generation after the second `_`, and the
K2-OpenHost tools require the form `cfs0_000_NNN` with a matching file name
(`08-loader-and-recovery.md` §6.3, which also covers how `NNN` decides whether a
stock update replaces OpenCFS).

**Loader state at entry.** The loader leaves SysTick running with its interrupt
(§5 of `08-loader-and-recovery.md`). The image unmasks interrupts only after VTOR
points at its own table, and its SysTick handler does nothing until `BoardApp`
exists; `Board::init()` then reprograms the tick.
