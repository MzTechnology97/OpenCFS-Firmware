# OpenCFS C++ core (from specification)

A portable C++17 implementation of the CFS application logic, written from the
behavioral specification in [`docs/spec`](../docs/spec). It contains no manufacturer
code. How it was produced, honestly stated:

- The specification was derived by analysing the original firmware (see
  `docs/spec/00-sources.md`). The code was then written against the specification
  and the host-visible contract, not translated from decompiled output.
- The same author did the analysis and wrote the code. This is a single-person
  "spec first" process, **not** a two-team clean room. Reviewers who need a strict
  clean room should treat the code as a reference model and re-derive from the spec.

## Layout

| Path | Content |
| --- | --- |
| `include/cfs/protocol.hpp`, `frame.hpp`, `lib/frame.cpp` | wire constants, CRC-8, frame codec, byte receiver (address filter, 120 ms timeout) |
| `include/cfs/hal.hpp` | hardware interfaces a board port implements (clock, feeders, hub, odometer, sensors, RFID, settings, link) |
| `include/cfs/config.hpp` | speeds, timeouts and spool geometry with the stock defaults |
| `include/cfs/box.hpp` | box state, latched error, slot events, and a motion flag independent of the state |
| `feeder.*` | load stages 0–7, unload phases, `0x12`/`0x13` feeds |
| `preload.*`, `spool.*` | insertion events, pretension, RFID revolution measurement, remaining-filament formulas |
| `print_tracker.*` | buffer refill while printing, `0x51`/`0x52` rules, runout |
| `dispatcher.*` | RS-485 command dispatcher |
| `auto_address.*` | A0–A3 enumeration (random discovery back-off, triple assign reply) |
| `usage.*` | filament usage accounting with live remaining-percent updates while printing |
| `supervisor.*` | blocked/overload supervision, reverse detooth, 10 s safety stop |
| `board/k2cfs/` | GD32F303 board port: own register map, GPIO/PWM/USART/ADC/SPI/soft-I2C, and HAL drivers (feeders with the ripple stall detector, hub speed PID with blocked/overload, sensors, MT6826, AHT30, LEDs, RS-485). Compiled as objects only. |
| `sim/` | deterministic filament-path simulator implementing the HAL |
| `tests/` | host tests (97 checks) |

The library uses no heap and no exceptions. Blocking operations call
`Clock::sleep_ms`, which the simulator turns into physics steps and an RTOS port
maps to a thread delay.

## Build

```sh
cmake -S cpp -B build-cpp -G Ninja
cmake --build build-cpp
./build-cpp/cfs_tests
```

## Status

Implemented:

- framing;
- auto-addressing A0–A3;
- commands `0x01`–`0x15`, `0x30` and `0x50`–`0x5B`;
- load/unload;
- preload and RFID estimate;
- usage accounting with **live remaining % during prints**: never writes tags, reports 0 % after a runout and `0xFF` once the spool is removed;
- print refill and runout;
- motor supervision and detooth.

Missing:

- `0x5C` (secure-element challenge) and `0x5D` (factory test);
- the K2-OpenHost `0x57` API;
- LED/LCD;
- in the board port: startup code, linker script and clock tree (no ELF/BIN by repository policy), the FM176xx RFID driver, the flash parameter store, and hardware confirmation of the MT6826 SPI framing and feeder direction.

**Nothing here produces a flashable image.** The repository's release gates
(`docs/ROADMAP.md`) apply unchanged.
