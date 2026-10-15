# Contributing to OpenCFS

This is an independent clean-room firmware implementation.

- Use newly written code or third-party code with compatible licenses,
  attribution and pinned origin. Project-owned C files use
  `SPDX-License-Identifier: Apache-2.0`.
- Never commit manufacturer firmware blobs, decompiled source
  reproductions, proprietary build artifacts, unique RFID data,
  credentials or private hardware dumps.
- Mark hardware facts as `source-derived`, `bench-measured`, or
  `unverified`. A chip or GPIO hypothesis is not a board qualification.
- Provide tests and a source trace for protocol and pin claims.
- Do not add a flash target, auto-update script, motor-actuating demo,
  new firmware BIN or bootloader commands to an early-stage PR.
- A CI green check means **software tests passed**, not that hardware
  can be operated safely.
- Keep new functional PRs focused, reviewed and initially in draft.

## Local host tests

```sh
cmake -S . -B build -DOPEN_CFS_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Freestanding ARM compile smoke test

```sh
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -std=c11 -Os \
  -ffreestanding -fno-builtin -Wall -Wextra -Werror -Wpedantic \
  -Iinclude -c src/safety_gate.c -o /tmp/opencfs-safety-gate.o
```

This produces only an **object file**, not an ELF or flashable BIN.
