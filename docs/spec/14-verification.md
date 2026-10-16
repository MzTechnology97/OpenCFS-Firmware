# Verification without hardware (OpenCFS)

What is checked before any hardware qualification, and how.

| Check | Where | What it proves |
| --- | --- | --- |
| Host simulation tests | `cpp/tests/test_main.cpp` (MSVC /W4 /WX, GCC with ASan/UBSan in CI) | Protocol, feeder stages, preload, RFID decoders, runtime config interlock, application loop, against a deterministic filament-path simulator |
| Deterministic fuzzing | `test_fuzz` in the same binary | 200 000 noise bytes into the frame receiver, 4 000 random frames through the dispatcher (commands that cannot start motion, plus unknown codes), 20 000 random buffers through the NDEF/JSON parsers, the parameter-page codec and the `0x60` encoder: no crash, no UB under the sanitizers |
| **Host-code conformance** | `cpp/tests/kalico_conformance.py` + `cfs_host_bridge` | The real Kalico host modules (`box_protocol.py`, `serial_485.py`, `box_cfs_runtime.py`, loaded from a Kalico checkout, not copied here) talk to OpenCFS running on the simulator. Auto-addressing from power-up, every `BoxDriver` query, a forced RFID read, and Runtime Config API v2 (INFO, DESCRIBE and GET for all 28 IDs, descriptor validation against the host catalogue, SET with the interlock, RESET) must pass Kalico's own envelope, CRC, length and payload checks |
| ARM object build | CI, `arm-none-eabi-g++ -Werror -Wpedantic`, with and without local vendor keys | Every core and board file compiles for the Cortex-M4F |
| **ARM image build** | `cpp/tools/build_image.py` (CI, and locally with Arm GNU Toolchain 14.3) | The image links at `0x08003000`; the vector table, the reserved header area, the size and the stack fit; no hosted C library code is linked (heap, stdio, exit) |
| **Emulated boot of the image** | `cpp/tests/image_boot_test.py` (Unicorn, Cortex-M4, minimal GD32F303 model) | The real binary, from its reset vector, with SysTick still running as the loader leaves it: no fault; VTOR, the clock register and SysTick set up as stock; feeders and hub braked; on a blank parameter page it erases and programs only `0x0807F800` and writes a valid record; the loader area and the image stay unchanged; auto-addressing (`A1` → one reply with the UID, `A0` → three replies) and a command at the new address, with valid CRC-8; a fault brakes every output and holds the hub PWM pin high |
| Static analysis | MSVC `/analyze` (local), `cppcheck` (CI, non-blocking for now) | No analyzer warnings on core and board sources |

Result on 2026-10-16: 293 host checks, 101 host-code conformance checks
(73 frames), 0 analyzer warnings; image 31 KiB flash, 3 KiB RAM, 16 emulated
image checks.

The emulator is a model: peripherals answer at once, analog inputs read 0, I2C
lines read released (no RFID reader, sensor or EEPROM answers), and interrupts
are taken between blocks of instructions. It shows that the binary starts,
configures what it should and runs the protocol. It does not show timing, motor
behavior or anything electrical.

## What this does not cover

- **Recorded stock traffic.** The Kalico logs on the reference printer do not
  contain raw RS-485 frames, so OpenCFS replies have not been compared byte for
  byte with stock replies. Conformance with the host code is checked instead.
  Capturing real traffic needs the bus or Kalico logging; both are left for
  later by choice.
- **Anything electrical:** feeder direction, hub PWM polarity and FG, current
  thresholds, odometer scale, RFID timings, flash page writes, the interlock with
  real motors. These need the hardware qualification step.

## Running it

```
cmake -S cpp -B build && cmake --build build && ctest --test-dir build
git clone --depth 1 --branch k2-pro-openhost https://github.com/MzTechnology97/kalico-k2pro /tmp/kalico
python3 cpp/tests/kalico_conformance.py --bridge build/cfs_host_bridge --extras /tmp/kalico/klippy/extras
python3 cpp/tools/build_image.py --out build-arm        # --toolchain DIR if not on PATH
pip install unicorn && python3 cpp/tests/image_boot_test.py build-arm/opencfs-unsealed.bin
```
