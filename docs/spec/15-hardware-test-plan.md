# First controlled hardware test plan (OpenCFS)

Status: **plan only. Nothing here has been run, and no OpenCFS image has been
flashed.** Every stage needs the owner's explicit go-ahead, and nothing below is
ever run unattended.

## Principles

- **Prove the path with a known-good image first.** The reference image is the
  firmware the box already runs well: Creality stock, or a K2-OpenHost build
  such as runtime-config v3.22. It is the first image through the custom path,
  and it is the recovery image. OpenCFS is flashed only once that path and the
  recovery have both worked.
- **One new thing per stage.** Each stage adds one class of risk: boot and bus,
  then sensors, then one actuator, then filament. Motion stays disabled in the
  build until stage 3.
- **Recovery is the custom path with the reference image**, and it needs nothing
  from the application (`08-loader-and-recovery.md` §6.1): power cycle, enumeration
  within the loader's 30 s window, reference image, `start_app`.
- **Abort** at any surprise: stop Klipper, switch the CFS off from the T113 (MCU
  rail) or at the mains, then recover to stock. Report before trying again.
- **Unchanged rules:** no RFID writes, Kalico untouched, vendor keys only on the
  owner's PC. An image built from the private tree embeds the keys and never
  leaves that PC and the printer.

## Prerequisites

| # | Item | State |
| --- | --- | --- |
| P1 | Slot B on the T113 with the corrected container check (k2-openhost-t113-bootstrap #13, programs update from the installer helper) | merged, not installed |
| P2 | Stock `cfs0_050_G32-cfs0_000_153.bin` on the CM5 with its SHA-256, and the box's UUID from `/tmp/.485_mcu_version` | to collect |
| P3 | Build variants INERT and BENCH (`13-application.md` §5): outputs locked in the drivers, motion commands answer `0x07`, the parameter page never written by the image; BENCH adds `0x7B` with SENSORS_ONLY / ACTUATOR_TEST. Identify the flashed one with `0x7A` before every stage | implemented, emulator-tested, not run on hardware |
| P4 | Version at `+0x200`: `cfs0_000_153` or another generation (`08` §6.3: decides whether a stock update replaces OpenCFS) | owner's decision |
| P5 | Watchdog: none, as stock, for stages 1–2; decide before stage 3 | owner's decision |
| P6 | A recorded stock baseline (stage 0.1) to compare against | to collect |

## Hardware results

| Date | Stage | Image | Result |
| --- | --- | --- | --- |
| 2026-10-17 20:31 | 1 | INERT `57ea823` | Flashed through the custom path (2.6 s, `update: ok`) and started. Then no device answered on the bus, steppers included: Kalico's first session sent 37 frames (253 bytes), received 0 frames and 236 bytes (its own echo), with no CRC error; the second session got 2 frames out of 43, with one CRC mismatch. No reply storm. No motion |
| 2026-10-17 20:35 | – | INERT in flash | A power cycle with `mcu_update` (CFS in its loader): every device answered |
| 2026-10-18 | bisection 1b | BUS-LED `bf398c3` | Only the loader's own LEDs: white sequence at power-up, then slot 4 red steady; none of the image's stage LEDs. **Root cause found:** the loader's jump reads MSP + 0x0C on the new stack (`08-loader-and-recovery.md` §5). Every OpenCFS image had MSP `0x20010000`, so the loader faulted reading `0x2001000C` before the jump and the CFS held the bus. Reproduced in the emulator with the real 2025 loader code (fault at `0x0800135A`). Fixed: MSP `0x2000FFE0`, and the image sets MSP again at entry |
| 2026-10-17 23:19 | bisection 1 | BUS-RELEASE `e0ba6efc` | Flashed (`update: ok`). Steppers not found, as with INERT. With no CFS connected the steppers work, so the CFS holds the bus electrically while this image runs. The loader checks only length and CRC (`0x08000A38`) and jumps 3 s after `start_app`; staying in the loader would not hold the bus |
| 2026-10-17 20:36 | 0.3 (recovery) | runtime-config v3.22 (reference) | Flashed through the custom path (14 s, `update: ok`). Bus, steppers and CFS back. Flashed again at 21:02 (`update: ok`); Kalico sees `firmware_features` 215, protocol 2 |

Stage 0 is therefore met with the box's own reference image, v3.22: the custom
path flashed an image, and the recovery brought the reference back through the
same path. Flashing Creality stock adds nothing on this box.

## Bus bisection (after the two INERT attempts of 2026-10-17)

Both INERT images stopped every device on the bus while OpenCFS ran; the
reference v3.22 brought it back each time. Before any further INERT, RELEASE or
BENCH attempt, the software part that breaks the bus is located step by step,
with motors never configured. Each step starts from the reference firmware,
verified working, and ends by restoring it.

| Phase | Image | What it does | Pass | Read-out |
| --- | --- | --- | --- | --- |
| 1 | **BUS-RELEASE** (`--variant busrel`, variant 4, about 1 KiB) | PA2 a GPIO held high from reset, the clock set up, then nothing: no USART, no frame sent, no other pin or peripheral touched, interrupts masked. A fault only releases the bus | The X/Y steppers are found by Kalico again. The CFS itself is expected offline | Pass: running minimal OpenCFS code does not disturb the bus. Fail: the cause lies in reset, clock or the loader handoff |
| 1b | **BUS-LED** (`--variant busled`, variant 5) | BUS-RELEASE plus one red slot LED per start-up stage: slot 1 = image running, slot 2 = PA2 high, slot 3 = 120 MHz clock up, slot 4 blinking = final loop. Slot 1 white = fault | Built after phase 1 failed on 2026-10-17 (steppers lost with BUS-RELEASE too; they work with no CFS connected) | All four, blinking: the code runs and holds PA2 high, so a high PA2 is not a released bus on this board. Fewer: the start-up stops at the first unlit stage |
| 2 | USART minimal (not built yet) | Phase 1 plus USART1 at 230400; then passive reception; then one controlled diagnostic reply; then enumeration | The steppers stay found at each sub-step | The first sub-step that loses them is the cause |

Electrical reference, without extra flashes: the CFS 5 V and the bus A/B
voltages measured with the v3.22 running. They are measured during a failure
only if the bisection gives no answer.

## Stage 0 — reference image only

**0.1 Baseline.** With stock 153 and Kalico as they are, record:
- the mcu_update log (`fw.cur_ver`, boot version, UUID);
- `/tmp/.485_mcu_version`;
- Kalico's box state, slot and hub masks, buffer state, encoder count;
- RFID records with one known Creality tag per slot;
- the front display values;
- one load and one unload per slot: time, and a short video showing the feeder and hub direction.

Read-only except for the load and unload, which the owner runs.

**0.2 Rehearse the custom path with the reference image** (stock 153 shown; on
a box that runs a K2-OpenHost build, use that build). With Klipper stopped
(host evidence):

```
k2oh-mcu-fw apply --cfs --cfs-image cfs0_050_G32-cfs0_000_153.bin \
  --cfs-sha256 <sha256> --cfs-uuid "<uuid>"
```

Pass:
- the log shows `F0 06`, `update_request`, the data, `update_end` and `start_app` ACK;
- `/tmp/.485_mcu_version` reports `update=ok` for the UUID;
- Kalico finds the box and the stage 0.1 queries match.

The box mode returns to standard, because `F0 06` erased the parameter page.

**0.3 Rehearse recovery.** Flash the reference image again through the same custom
path and time it, from the power cycle to `start_app`. The normal stock update
(`apply --cfs` with the real list) is **not** a recovery here: it updates only a
CFS whose generation is lower than the listed one or whose application is
invalid (`08-loader-and-recovery.md` §6.2). Stock, the K2-OpenHost
`cfs0_000_153` builds and OpenCFS all report 153, so it would leave any of them
in place. Recovery always means the custom path with the reference image (same
version allowed, no comparison in JSON mode).

**0.4 Back to the box's previous firmware.** If the box ran a K2-OpenHost build
before stage 0 (for example runtime-config v3.22), flash that build again
through the same path and check that Kalico sees its features
(`box_cfs_runtime.firmware_features`). Stage 0 ends with the box as it was.

**Preconditions for 0.2–0.4:** no print running, the box idle, the owner at the
printer. The helper refuses while printing, and stops Klipper itself only after
its own checks.

**Gate:** 0.2 and 0.3 each pass twice, then 0.4.

## Stage 1 — OpenCFS inert image: boot and bus

Build: inert variant, public tree (no vendor keys), sealed by the packager. It
must pass `image_boot_test.py` and the K2-OpenHost container check before it
goes to the CM5.

Before you start, remove the filament from all four slots.

1. Flash it through the stage 0.2 path. Pass: `start_app` ACK and `update=ok`.
   A NACK means an invalid image: stop and recover.
2. Start Klipper. Within 60 s Kalico must enumerate the box (`A1`, then `A0`
   with three replies) and the stage 0.1 queries must answer without CRC or
   length errors.
3. Leave it 10 minutes under Kalico's normal polling. Pass: no lost box, no
   reset, front display plausible (temperature and humidity against stock), no
   output moves.
4. Recover to stock (0.3) and repeat 0.1's queries.

Abort: the box missing after 60 s, repeated enumeration, or any movement.

## Stage 2 — sensors, read-only

BENCH image at SENSORS_ONLY (check `0x7A`: variant 3, level 1), filament handled by hand. For each slot, against the stage
0.1 baseline:

| Sensor | Check |
| --- | --- |
| inlet and hub sensors | inserting and removing the filament changes the slot and hub masks as stock |
| buffer full/empty | moving the buffer by hand changes the buffer state |
| MT6826 odometer | pulling a measured length (e.g. 100 mm) gives the stock count, within 2 % |
| RFID | `0x7B 20` (read in place) then `0x60`: the same tag gives the same readout as stock |
| EEPROM serial, unique ID, hardware straps | equal to stock |

Pass: every row matches. Recover to stock at the end.

## Stage 3 — one actuator at a time, no filament

BENCH image. ARM (`0x7B 01` with the box's unique ID) right before each step; it falls back to SENSORS_ONLY by itself after 5 s without frames, 120 s, a stall or an overload. The operator stays at the box, ready to cut power.

1. **Feeder direction.** Slot A, one 300 ms jog (`0x7B 10`) in each direction. Pass: it turns
   as in the stock video. Then slots B–D.
2. **Feeder current.** Jog with the gear free, then lightly held by hand. Pass:
   the stall detector fires only when held, and the ADC voltage is in the range
   assumed by `04-feeder.md`.
3. **Hub.** `0x7B 11`, low speed, up to 1 s. Pass: no full-speed start (PWM polarity), FG
   pulses counted, the brake stops it.
4. **Fault path.** `0x7B 30`: a real CPU fault while one feeder runs.
   Pass: everything brakes, the box stops answering, and a power cycle brings
   it back through the loader.

Pass: all four. Recover to stock.

## Stage 4 — filament, one slot, then four

Full image with motion enabled:

1. Load slot A to the hub and unload it, three times. Compare times and
   distances with the stage 0.1 baseline; pretension as stock.
2. The same for B–D, then a slot change.
3. A short print with a refill (runout on the active slot).

Pass: no stall, no unexpected motion, times within 20 % of stock.

## Stage 5 — soak

Two hours idle under Kalico polling, then one longer print with two slot
changes. Pass: no lost box, no reset, no unexpected motion.

## What is recorded at every stage

- the image SHA-256 and its build commit;
- `/tmp/mcu_update.log` and `/tmp/.485_mcu_version`;
- Kalico's log;
- the result of each check, with what was done when anything failed.

The results go into `14-verification.md`.
