# OpenCFS architecture: staged clean-room implementation

## Design boundary

The project is **new C source code**. No proprietary OEM firmware,
decompiled source reproduction, firmware image, private key, device
credential or third-party binary belongs in this repository.

The manufacturer firmware is only an **evidence source** for behavior,
hardware assignments and protocol interoperability. Every board fact
must distinguish original-ROM inference from actual measured hardware.

Current project state: a portable **C11 static library** containing an
unqualified-by-default software arbitration model, a receive-only
RS485 envelope decoder (opaque commands), and host tests.
It deliberately contains no MCU vector table, linker script, GPIO
writes, firmware updater or flashing facility.

## Planned layers

```text
CFS host / Kalico, tested synthetic RS485 input
           |
RX envelope decoder [HOST TESTED]; app semantics/TX NOT IMPLEMENTED
           |
transaction coordinator     [NOT IMPLEMENTED]
     +-----+-----+
     |           |
  RFID state   motor subsystem  [NOT IMPLEMENTED]
     +-----+-----+
           |
   shared arbitration model  [HOST TESTED ONLY]
           |
verified motor-stop evidence [NO HARDWARE PROVIDER]
           |
GD32 board port and RT-Thread  [NOT IMPLEMENTED]
```

RT-Thread (Apache-2.0) is the **intended** scheduler, not a dependency
in the first bootstrap. A new GD32 board support package and startup
file must be independently verified before compiling an ELF/BIN.

## Mandatory motor/RFID invariant

All possible motor-start paths (including GPIO commands, PWM commands,
ISRs, background feeders, resets and raw-serial protocol handlers)
must use a **single atomic exclusion domain**. RFID SET/RESET must
never proceed on an "IDLE" enum alone.

The current `safety_gate` is a deliberately small **reference model**:
- Boot starts `UNQUALIFIED`, denying all requests.
- Only a synthetic four-part evidence mask can transition to
  `VERIFIED_IDLE`; **no board implementation produces this mask**.
- C11 compare-and-exchange grants either one motor reservation or one
  RFID reservation, never both concurrently in the model.
- Releasing a reservation returns to `UNQUALIFIED`, not `VERIFIED_IDLE`.
- Fault latches until deliberate cold initialization.

This is **not a real safety interlock**. Caller-supplied evidence can
be wrong or stale. The model does not yet guard actual GPIO outputs,
motor power, all ISR entry points, DMA, watchdog resets or the whole
duration of a physical motor braking event. The target BSP must
prove a fresh independent motor-stop observation and an atomic
handoff to all hardware motion paths before this gate can be used.

## Resource and license policy

Use C11 and platform-independent modules where possible.
Cross-compile objects for Cortex-M4 without linking target firmware.
Bring in RT-Thread and a GD32 HAL only with pinned upstream source,
individual license checks, reproducible builds and an isolated bench.
No OEM app binary or ROM byte extraction is copied into OpenCFS.

The expected target *candidate* GD32F303VET6 is Cortex-M4,
512 KiB flash, 64 KiB SRAM. The physical chip and loader Flash/page
ownership are NOT independently authenticated.
