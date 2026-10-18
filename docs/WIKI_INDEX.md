# OpenCFS — Wiki / Documentation Index

This documentation lives in the version-controlled repository under `docs/`,
so the source, firmware code, hardware observations and changes remain reviewable
in one history. It is not the separately hosted GitHub Wiki repository.

## Start here

- [Architecture](ARCHITECTURE.md) — source layout, independent C11/C++ core
- [Roadmap](ROADMAP.md) — milestones completed and unresolved safety gates
- [Hardware and loader](spec/08-loader-and-recovery.md) — real app base
  `0x08003000`, bootloader recovery and loader/MSP handoff
- [RS-485](spec/03-rs485-protocol.md) — frame format, stock dispatcher,
  OpenCFS additions `0x57`, `0x7A` and `0x7B`
- [Application and build variants](spec/13-application.md) — RELEASE,
  INERT, BENCH, BUS-RELEASE/BUS-LED diagnostics, RAM/flash safety
- [Verification](spec/14-verification.md) — host tests, emulator, limits
- [Hardware test plan and measured results](spec/15-hardware-test-plan.md)
  — October 17–18 firmware trials, successful v3.22 rollback, confirmed
  initial-MSP loader fault and corrected hardware boot

## CFS runtime configuration and firmware families

Two firmware families are recognized with read-only command `0x7A`:

| Identity | Firmware | API | Compatible parameter profile |
| --- | --- | --- | --- |
| `OCFS` | Independent OpenCFS RELEASE/INERT/BENCH | OpenCFS `0x57` v2, features `0x2B` | OpenCFS-specific descriptor/units and interlock only |
| `K2OH` | Patched Creality G32/153 firmware | v3.23 CFG4 `0x57`, features `0xD7` + revision `v323-cfg4` | **Only matching firmware from k2-cfs-rfid-tools** |
| None | Stock / unknown | not guaranteed | No CFG4 writes |

**Do not treat `OCFS` and `K2OH` as interchangeable.** Some OpenCFS
wire values (e.g. ID 8 in 10-ms polls) have a different unit contract from
K2OH CFG4 (true milliseconds). The Kalico host must inspect identity, feature
byte, parameter descriptor and profile before sending any value.

References:

- [OpenCFS runtime API and parameter semantics](spec/12-runtime-config.md)
- [K2OH CFG4 full Italian guide — 28 parameters, writable flags and defaults](https://github.com/MzTechnology97/k2-cfs-rfid-tools/blob/main/docs/CFS_V323_PARAMETER_GUIDE_IT.md)
- [K2OH firmware compatibility rules](https://github.com/MzTechnology97/k2-cfs-rfid-tools/blob/main/docs/CFS_FIRMWARE_COMPATIBILITY.md)
- [Kalico v3.23 firmware-family detection and CFG-only startup](https://github.com/MzTechnology97/kalico-k2pro/blob/k2-pro-openhost/docs/CFS_RUNTIME_V323_CFG4.md)
- [Installer Helper custom-CFS recovery path](https://github.com/MzTechnology97/k2-openhost-installer-helper/blob/main/firmware/custom-cfs/README.md)

## Confirmed hardware failure and resolution

Early OpenCFS INERT images held the bus with no replies. The root cause was
not established by the original PA2/echo hypothesis. Hardware bisection on
18 October 2026 reproduced an **initial MSP error**: with MSP at
`0x20010000`, the original Creality loader reads `MSP+0x0C`, faulting at
an unmapped address before entering the application. Moving initial MSP to
`0x2000FFE0` allowed the loader to launch OpenCFS and restored RS-485
and stepper enumeration in subsequent INERT tests.

This is stage-1 boot evidence only, **not production qualification**.
No CFS firmware flash should be initiated by documentation, CI, or a host
agent without the owner's deliberate action.
