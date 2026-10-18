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
| `param_store.*` | stock parameter page codec (CRC-16/XMODEM, keys 5 and 6, load policy) |
| `aes128.*`, `creality_tag.*` | AES-128 block cipher and the stock Creality tag Key A derivation / block decryption (keys are parameters) |
| `sha256.*` | SHA-256, HMAC-SHA256 and HKDF-SHA256 (Bambu key derivation) |
| `runtime_config.*` | K2-OpenHost Runtime Config API v2 (`0x57` 0x10..0x14, 28 IDs, RAM only) with the MCU motion interlock (`docs/spec/12`) |
| `app.*` | the cooperative application loop that replaces the stock threads and soft timers, with background duties during long routines (`docs/spec/13`) |
| `tags/tag.*`, `tags/decoders.*` | read-only multi-vendor tag pipeline: Creality, Bambu, configured MIFARE keys (QIDI), NFC Type 2 / NDEF (OpenSpool), per-UID routing, command `0x60` payload (`docs/spec/11`) |
| `board/k2cfs/` | GD32F303 board port: own register map, GPIO/PWM/USART/ADC/SPI/soft-I2C, and HAL drivers (GPIO feeders with the ripple stall detector, hub speed PID with blocked/overload, sensors, MT6826S with the stock framing, AHT30, LEDs, RS-485, stock-compatible flash parameter page, TM1621B front display, read-only 24C16 EEPROM (production serial number), hardware-version straps, FM17622 read-only RFID driver as a tag session, `vendor_decoders.hpp` building the decoders from git-ignored `local/creality_keys.hpp` (`tools/re/extract_tag_keys.py`, from your own firmware image) and `local/vendor_keys.hpp` (from `vendor_keys.example.hpp`)). |
| `board/k2cfs/image/` | **EXPERIMENTAL, not qualified on hardware.** Linker script at `0x08003000`, vector table, reset and fault handling, stock clock set-up, entry; RELEASE, INERT and BENCH variants (`docs/spec/13` §4–5) |
| `board/k2cfs/bench.*` | BENCH build only: command `0x7B` with SENSORS_ONLY and ACTUATOR_TEST levels, bounded jogs, tag read in place, fault test |
| `sim/` | deterministic filament-path simulator implementing the HAL |
| `tests/` | host tests and deterministic fuzzing (314 checks); `kalico_conformance.py` drives OpenCFS with the real Kalico host modules; `image_boot_test.py` boots the linked image in an emulator (`docs/spec/14`) |
| `tools/host_bridge.cpp` | `cfs_host_bridge`: OpenCFS on the simulator behind a line-based frame interface, for host-code conformance tests |
| `tools/build_image.py` | builds and checks the RELEASE, INERT and BENCH images: ELF, map, unsealed binary, and with `--seal` a named `…-OpenCFS-<VARIANT>-<rev>.bin` with the Creality header. Never flashes |

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
- for an image: startup code, vector table and linker script (no ELF/BIN by repository policy; the clock setup and the integration contract are in `docs/spec/13`), and hardware confirmation of the feeder direction, the flash parameter writes and the RFID timings. The MT6826S framing is confirmed by the stock code and a public capture, not yet on our own board.

**Nothing here produces a flashable image.** The repository's release gates
(`docs/ROADMAP.md`) apply unchanged.
