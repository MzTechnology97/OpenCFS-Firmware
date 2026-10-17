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

**RS-485 bus.** The transceiver has no direction pin and follows TX (PA2)
(`10-board-port.md` §2), so a low or floating PA2 holds the bus for every
device on it, steppers included. The image therefore:
- makes PA2 a GPIO output held high as its first action after reset. The
  loader resets GPIOA before the jump, so PA2 floats until then;
- hands PA2 to USART1 only after the USART runs and idles high;
- sets up the USART before any other peripheral;
- holds PA2 high again in the safe state, so a stopped box is silent and the bus
  is free;
- turns the receiver off while it sends and drops what it caught;
- ignores frames that are replies, not requests (`03-rs485-protocol.md` §2.1).

On 2026-10-17 the first INERT image on the reference printer (`57ea823`, without
these measures) broke the bus after start-up: no device answered, steppers
included; nothing moved. While the CFS sat in its loader, everyone answered
again, and the image was replaced through the custom path.

Kalico's counters point away from a reply storm: 37 frames sent, 0 received,
and 236 bytes received for 253 sent, that is, its own echo. The likeliest
reading is that the image never brought its USART up. PA2, left floating by the
loader, then disturbed the bus. A box that drives its transmitter from a stuck
TX line can block every device it is wired to, while each host still hears
itself. The root cause is not proven.

In the emulator, that image:
- left PA2 floating before the clock;
- handed PA2 to a USART that was not yet running;
- with a receiver that hears its own transmission, entered an endless reply
  exchange (2 184 frames for one request).

All three are closed in the current image and covered by `image_boot_test.py`.
Why the start-up did not reach the USART is still open.

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

## 5. Build variants: RELEASE, INERT, BENCH (EXPERIMENTAL)

`cpp/tools/build_image.py --variant release|inert|bench|all [--seal]` builds each
variant in its own directory. With `--seal` the file name carries the variant:
`cfs0_050_G32-cfs0_000_153-OpenCFS-<VARIANT>-<revision>.bin`, plus a `.sha256`.
The Creality version at `+0x200` stays `cfs0_000_153` for all three, as the
loader and K2-OpenHost expect (`08-loader-and-recovery.md` §6.3). The sealed
bytes are identical to the K2-OpenHost Build-Tools packager's.

| | RELEASE | INERT (stage 1) | BENCH (stages 2–3) |
| --- | --- | --- | --- |
| Feeder and hub outputs | normal | locked in the drivers: a run() brakes | locked, except one bounded jog after ARM |
| Hub PWM pin (PE13) | TIMER0 | plain GPIO held high (off), never given to TIMER0 | GPIO high, TIMER0 only during a hub jog |
| Motion commands (`0x04`, `0x06`, `0x07`, `0x10`..`0x13`, `0x0D` sub 2/3) | normal | status `0x07` | status `0x07`, also while armed |
| Preload, pretension, scan on insertion; detooth | normal | never | never |
| Parameter page `0x0807F800` | written as stock | never erased or written by the image (RAM only) | same as INERT |
| `0x7B` test commands | not in the image | not in the image | present |
| `0x7A` identity | variant 1 | variant 2, flags 3 | variant 3, flags 3, bench level |

A fourth, diagnostic variant, **BUS-RELEASE** (`--variant busrel`, variant 4,
`OpenCFS-BUSREL`), is phase 1 of the bus bisection (`15-hardware-test-plan.md`).
It holds PA2 high from reset, sets up the clock, and stops there: no USART, no
frame, no other pin, interrupts masked. Its fault handler only releases the bus.
It cannot answer `0x7A`; its identity is in the file at `+0x220`.

The loader's `F0 06` still erases the parameter page at every update; INERT and
BENCH only add no writes of their own.

**How the lock is enforced.** At three levels, so one mistake is not enough:
- **Drivers:** `Feeders::set_allowed()` and `Hub::set_allowed()` refuse to drive
  anything not allowed and brake instead. `BoardApp` sets them before the first
  peripheral set-up.
- **Dispatcher:** `starts_motion()` commands answer `0x07`.
- **Application:** no preload step, and the supervisor never starts a corrective
  move. It still stops motors.

**Identity.**
- `0x7A`, answered by every build, also while a routine runs:
  `"OCFS"`, format 1, variant, bench level, flags, revision[12], version[12]
  (`03-rs485-protocol.md` §8).
- The same values, in a 40-byte block at image `+0x220`:
  `"OCFSINFO"`, format 1, variant, flags, 0, revision[12],
  name[16] (`OpenCFS-RELEASE`, `-INERT`, `-BENCH`).

  A host tool can tell the variant from the `.bin` file before it is flashed,
  and from the box afterwards.

**BENCH levels.** Every start is **SENSORS_ONLY**: every actuator is locked, and
reads work (sensors, encoder, environment, tags in place). **ACTUATOR_TEST**
comes only from an explicit ARM and lasts until:
- DISARM;
- a stall, an overload or a travel limit during a jog;
- 5 s without any frame to this box;
- 120 s after ARM;
- a reset.

Nothing about the level is stored.

| `0x7B` sub | Request | Reply | Level |
| --- | --- | --- | --- |
| `00` STATUS | – | level, last result, seconds left armed | any |
| `01` ARM | `'A' 'R' 'M'`, unique ID bytes 8..11 | level | to own address only; refused (`0x03`) unless at rest |
| `02` DISARM | – | level | any |
| `10` FEEDER_JOG | slot, dir (0 fwd, 1 rev), ms u16 (1..500) | result, ms u16, feeder voltage mV u16, odometer 0.1 mm i16 | ACTUATOR_TEST |
| `11` HUB_JOG | dir, speed (1..64), ms u16 (1..1000) | result, ms u16, FG pulses u16, rps ×10 u16, odometer 0.1 mm i16 | ACTUATOR_TEST |
| `20` TAG_READ | slot | found (bit 0 UID, bit 1 record), UID length, UID[10]; the readout then through `0x60` | any, no motion |
| `30` FAULT_TEST | slot | `0x00`, then that feeder runs 50 ms and a real CPU fault (`udf`) is raised | ACTUATOR_TEST |

Result codes: 0 done, 1 stalled, 2 overload, 3 travel limit (more than 30 mm on
the odometer).

Every jog:
- moves one output, with every other output locked;
- starts only with everything at rest and the box idle;
- has a hard time limit;
- runs the hub PWM capped at 96/255;
- brakes at the end, whatever the outcome.

A jog or the fault test at SENSORS_ONLY is refused (`0x07`). The bus is
not served during a jog of at most 1 s; frames wait in the receive buffer.

**Verification.** `tests/image_boot_test.py` runs on each linked binary
(`14-verification.md`).
