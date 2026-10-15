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
| `sim/` | deterministic filament-path simulator implementing the HAL |
| `tests/` | host tests (64 checks) |

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
- commands `0x01`–`0x14` and `0x50`–`0x56`;
- load/unload;
- preload and RFID estimate;
- print refill and runout.

Missing:

- auto-addressing `A0`–`A3`;
- `0x15`, `0x30`, `0x5B`–`0x5D`;
- the K2-OpenHost `0x57` API;
- RFID usage accounting;
- motor current supervision and detooth;
- LED/LCD;
- the GD32F303 board port.

**Nothing here produces a flashable image.** The repository's release gates
(`docs/ROADMAP.md`) apply unchanged.
