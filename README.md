# OpenCFS Firmware

**Independent, open-source firmware project for the Creality CFS controller.**

> **NOT QUALIFIED ON HARDWARE. DO NOT FLASH.** [`cpp/board/k2cfs/image`](docs/spec/13-application.md)
> links an **experimental** application image for the Creality loader (start-up,
> clock, vector table, linker script). It boots in an emulator, but motors, sensors,
> RFID, timing and recovery have not been tested on a physical CFS. No binary is
> published. Never install a build artifact on a physical CFS before the hardware
> qualification.

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
  and nothing is flashed.**
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
A runtime GPIO driver is intentionally absent.

## Architecture and research

Start with [RS485 decoder evidence](docs/RS485_RX_ENVELOPE.md), [architecture](docs/ARCHITECTURE.md), [roadmap](docs/ROADMAP.md),
[hardware evidence manifest](boards/k2-cfs-gd32f303ve/board-manifest.json),
[reference research](docs/REFERENCES.md) and
[contributing rules](docs/CONTRIBUTING.md).

The target chip *candidate* is GD32F303VET6 (Cortex-M4).
Physical silicon confirmation, bootloader boundaries, motor feedback,
IRQs and CFS protocol interoperability are still unqualified.
RT-Thread is a likely future RTOS dependency; it has **not** been
imported into this project.

Source code created for OpenCFS uses **Apache-2.0**.
Do not upload or copy proprietary Creality binaries or reconstructed
proprietary source code into this repo.

Development and reviews proceed via draft pull requests on feature branches.
