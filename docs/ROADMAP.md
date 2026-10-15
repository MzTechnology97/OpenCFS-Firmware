# OpenCFS milestone plan

All milestones below are explicit; a green CI is **not** authorization
to flash a real CFS.

| Phase | Result required | State |
| --- | --- | --- |
| 0 | Research links, original hardware source provenance, no OEM redistribution | In progress |
| 1 | Reproducible portable C11 core, desktop tests, ARM object compilation, CI | Bootstrap PR |
| 2 | Clean-room protocol frame tests from sanitized real captures; strict bounds, CRC and timeout handling | Not started |
| 3 | Host-simulated feeder and RFID transaction state machines, replay and fault injection | Not started |
| 4 | RT-Thread and GD32 HAL BSP from compatible, pinned upstream licenses; clocks, peripherals, interrupt routing | Not started |
| 5 | Physical idle sensing, cross-task and ISR atomic motion interlock, brake/stop failure behavior | Not started |
| 6 | Loader app-length/page protections, authenticated recovery and rollback, complete target memory map | Blocked |
| 7 | Isolated spare-CFS bench smoke tests, capture/replay, power loss, RFID compatibility and end-to-end regression | Blocked |
| 8 | Reviewed reproducible target application artifact; hardware qualification evidence | Blocked |

## Immediate next implementation

1. Build a sanitized, versioned RS485 packet capture corpus with no
   customer, printer or unique RFID identifiers.
2. Implement a byte-stream framing parser with a **known** header,
   CRC, length bounds and resynchronization documented by captures.
   Never infer a wire format from a single sample.
3. Add a deterministic host simulator of four motors with failure
   injection. Do not drive physical pins.
4. Separate read-only hardware examination from any flash proposal.
5. Choose an exact upstream RT-Thread revision and review the licenses
   of the GD32 BSP/HAL before importing third-party code.

## Hard release blockers

We currently lack an independently verified GD32 chip identification,
loader erase and application size limits, recovery/rollback, safe board
startup, fully observed motor feedback, ISR/task ownership and protocol
coverage. No executable CFS firmware or flashable binary shall be
published until those gates are independently closed.
