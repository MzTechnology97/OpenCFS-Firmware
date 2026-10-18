# OpenCFS Firmware

**Independent, open-source firmware project for the Creality CFS controller.**

> **EXPERIMENTAL — NOT PRODUCTION-QUALIFIED.** Some OpenCFS INERT images
> were physically flashed during 17–18 October 2026, and the v3.22
> patched-stock firmware successfully recovered the device each time.
> Initial INERT images blocked the shared RS-485 bus. The confirmed root
> cause was the bootloader reading 12 bytes beyond an initial MSP of
> `0x20010000` before jumping into the app; the corrected initial MSP is
> `0x2000FFE0`. Subsequent stage-1 INERT builds restored CFS and motor
> discovery; see [hardware evidence and exact revisions](docs/spec/15-hardware-test-plan.md).
> Hardware qualifications for feeder motion, RFID, BENCH and RELEASE remain open.
> Do not treat passing emulator tests as authorization for production deployment.

**Identity and compatibility.** OpenCFS identifies with command `0x7A`
and magic `OCFS` (variant RELEASE/INERT/BENCH), distinct from the
patched Creality/K2-OpenHost `K2OH` identity. OpenCFS's Runtime Config API
`0x57` reports feature `0x2B`. The wire protocol shares the family of
API v2, but some stock timebase parameters (especially IDs 8, 20 and 22)
use different raw units than K2OH v3.23 CFG4. **Kalico must recognise both
families and never apply the wrong firmware's parameter profile.**

[Wiki-style documentation index](docs/WIKI_INDEX.md) ·
[Runtime configuration](docs/spec/12-runtime-config.md) ·
[Hardware test plan](docs/spec/15-hardware-test-plan.md)

## What exists now

- Portable **C11** core library with a fail-closed **motor/RFID arbitration
  reference model** (not a physical safety system).
- Deterministic host tests for startup rejection, fault latching, synthetic
  idle requalification and 16-thread concurrent motor/RFID claim exclusion.
- **Receive-only bounded RS485 decoder** with original-research `F7`
  framing and CRC-8; command bytes are opaque and never executed.
- Offline tests for split packets, malformed lengths, CRC rejection,
  payload-embedded synchronization bytes, timeouts and large frames.
- Cross-compilation of the **isolated core object** for ARM Cortex-M4.
- Source-only board evidence manifest, clean-room contribution rules and
  [staged development roadmap](docs/ROADMAP.md).
- CI that runs host tests and sanitizer tests, verifies ARM object compilation, links
  the experimental image and boots it in an emulator
  ([`docs/spec/14`](docs/spec/14-verification.md)). **No ELF/BIN/HEX is published
  and physical INERT-only bench tests are documented.**
- **Behavioral specification** of the stock CFS application `cfs0_000_153` in
  [`docs/spec`](docs/spec/00-sources.md): RS-485 commands and statuses, load/unload
  stages, preload/RFID and the remaining-filament estimate, motors and supervision,
  memory map, loader image header, loader update protocol and the GD32 peripheral/pin map for a board port.
- A **C++17 reference implementation** of that specification in [`cpp/`](cpp/README.md),
  with a filament-path simulator and host tests. It is heap-free. The core does not
  touch hardware; the GD32F303 board port and the image in `cpp/board/k2cfs` do.
- Offline **analysis tools** in [`tools/re`](tools/re/README.md); none of them
  redistribute manufacturer bytes.

## Build and test on Linux

Requires CMake, a C11 compiler and pthreads.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/test_board_manifest.py
```

Optional Cortex-M4 compilation (not linking):

```sh
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -std=c11 -Os \
  -ffreestanding -fno-builtin -Wall -Wextra -Werror -Wpedantic \
  -Iinclude -c src/safety_gate.c -o /tmp/opencfs-safety-gate.o
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -std=c11 -Os \
  -ffreestanding -fno-builtin -Wall -Wextra -Werror -Wpedantic \
  -Iinclude -c src/rs485_rx.c -o /tmp/opencfs-rs485-rx.o
```

The synthetic `OCFS_EVIDENCE_*` flags cannot currently be produced from
trusted real CFS hardware feedback. The module is **only a logic test
harness**; setting the flags does not prove that motors are stopped.
A preliminary GD32 board port exists, but production motion safety is unqualified.

## Architecture and research

Start with [RS485 decoder evidence](docs/RS485_RX_ENVELOPE.md), [architecture](docs/ARCHITECTURE.md), [roadmap](docs/ROADMAP.md),
[hardware evidence manifest](boards/k2-cfs-gd32f303ve/board-manifest.json),
[reference research](docs/REFERENCES.md) and
[contributing rules](docs/CONTRIBUTING.md).

The target chip *candidate* is GD32F303VET6 (Cortex-M4).
Physical silicon confirmation, bootloader boundaries, motor feedback,
IRQs and CFS protocol interoperability are still unqualified.
The current image uses a cooperative bare-metal application; RT-Thread has not been imported.

Source code created for OpenCFS uses **Apache-2.0**.
Do not upload or copy proprietary Creality binaries or reconstructed
proprietary source code into this repo.

Core, protocol, application and source-linked image contributions have been merged into main; physical tests remain controlled and documented.
