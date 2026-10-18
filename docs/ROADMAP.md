# OpenCFS milestone plan

All milestones below are explicit; a green CI is **not** authorization
to flash a real CFS.

| Phase | Result required | State |
| --- | --- | --- |
| 0 | Research links, original hardware source provenance, no OEM redistribution | In progress |
| 1 | Reproducible portable C11 core, desktop tests, ARM object compilation, CI | Implemented and merged |
| 2 | Bounded RS485 receive framing, dispatcher and host evidence | Protocol and dispatcher implemented; physical stage-1 verified, wider coverage pending |
| 3 | Host-simulated feeder/RFID state machines and failure injection | Reference simulator implemented; complete hardware regression pending |
| 4 | GD32 BSP, clocks, startup, physical pin mapping | Bare-metal board port + image implemented; loader/MSP start fixed and stage-1 boot verified |
| 5 | Physical idle sensing, cross-task and ISR atomic motion interlock, brake/stop failure behavior | Not started |
| 6 | Loader app-length/page protections, recovery and rollback, complete target memory map | Reference v3.22 recovery tested; full partition and interruption scenarios still open |
| 7 | Controlled hardware stage-1 bus smoke tests, power-loss recovery, RFID and end-to-end regression | INERT bus smoke/recovery completed on owner CFS; full motion/RFID work open |
| 8 | Reviewed reproducible target application artifact; hardware qualification evidence | Blocked |

## Immediate next implementation

1. Build a sanitized, versioned RS485 packet capture corpus with no
   customer, printer or unique RFID identifiers.
2. Expand the **implemented receive-only** frame decoder with actual
   sanitized MCU captures, direction/status validation, CRC cross-checks,
   fuzzing and resynchronization statistics. Never infer all commands
   or loader semantics from the version-query sample.
3. Add a deterministic host simulator of four motors with failure
   injection. Do not drive physical pins.
4. Separate read-only hardware examination from any flash proposal.
5. Choose an exact upstream RT-Thread revision and review the licenses
   of the GD32 BSP/HAL before importing third-party code.

## Hard release blockers

Stage-1 MCU startup and reference-image recovery have hardware evidence,
but exact flash/page ownership under interruption, full physical motor
feedback, actuator safety, vendor RFID read/write protection and sustained
bus qualification remain open. No **production-qualified** firmware shall be released until all safety,
interruption-recovery, motion and RFID gates are closed. Experimental target
images and controlled owner-led stage-1 tests are documented separately in
[spec/15](spec/15-hardware-test-plan.md). The loader-MSP fault on the first
INERT images was isolated and corrected with real hardware evidence.
