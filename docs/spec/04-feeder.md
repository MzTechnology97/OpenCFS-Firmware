# Filament path: load, unload, print tracking

Behavioral specification of the CFS feeding logic in application `cfs0_000_153`.
Function names are Ghidra labels (`tools/re/ghidra/names.tsv`). Timing values are the
stock defaults; v3.20+ makes some of them adjustable through runtime IDs.

## 1. Mechanics and sensors

```
 slot A..D  ──feeder motor (brushed, per slot)──▶ inlet sensor ─▶ ┐
                                                                    ├─▶ hub (BLDC + magnetic odometer) ─▶ hub sensor (per slot path) ─▶ PTFE ─▶ buffer (2 switches) ─▶ toolhead
```

| Signal | Pins (slot A, B, C, D) | Meaning |
| --- | --- | --- |
| Inlet sensor | PD7, PD6, PD5, PD4 | low = filament present at the slot inlet |
| Hub sensor | PE10, PE9, PE8, PE7 | low = this slot's filament occupies the hub path |
| Buffer | PD8 (*full* switch), PD9 (*empty* switch) | state = `(!PD8) \| (!PD9)<<1`: 1 = full, 2 = empty, 0 = in between |
| Odometer | MT6826 on SPI1 | hub-side filament position in mm; **feeding makes it decrease** |

Sensor reads are debounced by requiring N consecutive equal samples (typically 5 or
10 samples at 10 ms; the buffer needs 10 samples at 1 ms).

## 2. Speed table

Stored in RAM at boot (no persistence). Speed units are driver PWM levels (0..255).

| Index | K2-OpenHost ID | Use | Default |
| --- | --- | --- | --- |
| 1 | 0 `feeder_forward_speed` | slot feeder forward | 255 |
| 2 | 1 `hub_forward_speed` | hub forward (normal, refill, recovery) | 100 |
| 3 | 2 `hub_transition_speed` | hub forward toward the buffer (stage 1/4, post-stage-0 push) | 200 |
| 4 | – | hub forward for the final push into the extruder (stage 7) | 150 |
| 5 | 3 `hub_insert_speed` | hub while the slot feeds up to the hub sensor | 155 |
| 6 | 4 `feeder_reverse_speed` | slot feeder reverse | 255 |
| 7 | 5 `hub_reverse_speed` | hub reverse | 80 |

`0x09` lets the host change the manual-motion values (feeder/hub × forward/reverse).

## 3. Common building blocks

- **Run slot feeder** `(slot, dir, speed)` / **stop feeder(s)**.
- **Run hub** forward/reverse at a speed / **stop hub**.
- **Wait hub sensor** `(slot, timeout_10ms, wanted_level, samples)`: returns
  success when the sensor shows `wanted_level` for `samples` consecutive 10 ms reads
  before `timeout` expires.
- **Odometer watch**: sample every 20 ms; "moving" means the change since the last
  sample exceeds 0.01 mm.
- **Finish**: on success the box returns to IDLE; on error code `e` (other than
  1 INVALID and 3 BUSY) the box enters ERROR with `error = e`, and the slot
  indicator shows the error.

## 4. Load (`0x10`, `cmd_load_stage`)

Request `(mask, stage, arg)`. The host (Kalico) drives stages 0 → 4 → 5 (poll) → 6 → 7.

### Stage 0 — slot to hub (`cmd_parse_out_stage1`)

1. **Precondition** (`feed_precheck`):
   - the box state is not IDLE → BUSY (`0x03`);
   - a preload/pretension is running → BUSY (`0x03`);
   - otherwise the state becomes RELOAD (3);
   - the inlet sensor must read *present* 5 times in a row within 30 attempts
     → else `0x08` SLOT_EMPTY;
   - any **other** slot whose hub sensor shows filament is retracted first
     (see unload phase 1). Its error is returned and the offending slot recorded.
2. **Feed to hub sensor** (`feed_until_hub_sensor`): feeder forward [1] together with
   hub [5]; wait up to **7 s** for the hub sensor to read *present* (10 samples).
   Timeout → stop everything, `0x05` STAGE0_SENSOR_TIMEOUT.
3. **Push through the hub** (`feed_hub_outside`): keep feeding until the odometer
   has advanced **20 mm** within **2 s**. Timeout → `0x09` STAGE0_ODOMETER_TIMEOUT.
   A variant (mode 1, used by recovery paths) retries 3 times with a short
   reverse/forward wiggle and 4 s per attempt.
4. Stop the feeder (the hub keeps the filament).
5. On success: hub forward at [3], enable the buffer path, wait **3.2 s**
   (`hub_transition_wait_ms`), then keep going in 10 ms steps while the
   transition flag is set, then stop.
6. Reply status + 1 byte (the slot index).

### Stage 1 — hub feed until the filament stops (internal)

Hub forward [3]; sample the odometer every 20 ms for at most **25 s**.
- Fed more than **3000 mm** → stop, `0x0B` OVERTRAVEL.
- No movement for **25 samples (500 ms)** → done (filament reached a stop).

### Stage 2 — hub start with movement check (internal)

Hub forward [2]; check for movement once a second, up to 3 s
(`feed_recovery_timeout_ms`). No movement → `0x0A`. Then continue as stage 1.
Mode 1 adds up to 3 attempts, each preceded by 500 ms of hub reverse [7].

### Stage 3 — finish

Return to IDLE and reply OK.

### Stage 4 — feed to the buffer (asynchronous)

1. Reply OK at once, set state RELOAD, then hub forward [3].
2. Loop every 20 ms for at most 1250 samples (**25 s**).
   While the loop runs, the RS-485 thread is busy with it. It reads incoming
   frames directly and only understands LOAD frames:
   - stage 5 → reply OK + odometer position (float, big-endian);
   - stage 6 → leave the loop and go to the buffer check;
   - any malformed frame → reply status `0x02`;
   - anything else is discarded (the input is drained).
3. Errors: fed more than 3000 mm → `0x0B`; odometer still for 500 ms → `0x0C`
   ODOMETER_STALLED; 25 s elapsed → `0x0A` FEED_TIMEOUT. The error is kept and
   reported by the next stage 5 reply.
4. On stage 6 inside the loop: wait up to **3 s** for *buffer full*; stop the hub.
   Full → `0x00`, otherwise `0x0D` BUFFER_NOT_FULL. Finish and reply to the stage 6 frame.

### Stage 5 — progress poll

Reply `(stored stage-4 result, odometer float BE)` and clear the stored result.

### Stage 6 — buffer full check (standalone)

If the buffer is not full within 15 ms: hub forward [2] for up to 3 s until full,
else `0x0D`. Stop the hub.

### Stage 7 — push into the extruder

`arg` = distance in mm (Kalico sends 3). Hub forward [4]; succeed as soon as the
odometer has advanced `arg` mm; give up after 15 samples (**300 ms**) →
`0x0E` STAGE7_NO_MOTION.

## 5. Unload (`0x11`, `cmd_unload`)

Request `(mask, phase)`. `mask = 0` applies the phase to every slot whose hub sensor
shows filament, one at a time, stopping at the first error.

**Precondition:** the state must be IDLE or PRELOAD, else BUSY. The selected slot
must show filament at its inlet, else ERROR with `0x16` UNLOAD_NO_FILAMENT. With
mask 0 and no slot loaded the result is `0x18` UNLOAD_ALL_EMPTY. The state is then
RELOAD for the duration.

### Phase 0 — empty the buffer (`unload_buffer`)

Feeder reverse, hub reverse [7]; wait up to **5 s** for *buffer empty*; stop the hub.
Timeout → `0x13` UNLOAD_BUFFER_TIMEOUT.

### Phase 1 — retract the slot (`cmd_parse_back_stage2`)

Also closes the slot's RFID usage accounting before starting.

1. The inlet sensor must show filament; otherwise `0x17` UNLOAD_INLET_CLEAR.
   This is the reverse of the hint text in Kalico `box_protocol.py`, which says
   "inlet still showed filament".
2. **Pull back through the hub** (`pull_back_hub`): hub reverse [7] with feeder reverse [6].
   Sample every 20 ms for at most 2000 samples (**40 s**):
   - done when the hub sensor reads *clear* 5 times in a row;
   - the motor-blocked monitor fires → stop everything, `0x19` UNLOAD_MOTOR_BLOCKED;
   - no odometer movement for 150 samples (3 s) leads to a decision
     (with up to 2 nudges in the recovery variant);
   - 40 s elapsed → `0x1A` UNLOAD_ODOMETER_TIMEOUT.
3. **Clear the hub path sensor** (`pull_back_hub_pe`):
   - If the sensor shows filament, keep reversing and wait up to **7 s** for 10
     *clear* samples, else `0x14` UNLOAD_HUB_PE_TIMEOUT. The recovery variant makes
     3 attempts, each re-feeding 20 mm first.
   - If it is already clear: stop the feeder, confirm for 0.5 s, and reverse again if needed.
   - Wait 300 ms, then stop the slot feeder.

## 6. Other motion commands

| Cmd | Behavior |
| --- | --- |
| `0x12` | `cmd_feed_process_3(slot, mode, speed, distance_mm)`: feed a requested distance (details in `05-print-tracking.md`, **to be completed**). |
| `0x13` | Precondition as stage 0, then feed slot → hub (steps 2–3 of stage 0) and run stage 1. |
| `0x06` / `0x07` | Manual feeder/hub motion at the manual speeds; refused while busy. |

## 7. Print tracking (`0x04` mode 0, thread `print_check`)

While PRINT is active, the buffer switches are interrupt-armed and the
`print_check` thread keeps the buffer between *empty* and *full*:

- **buffer empty** → run the current slot's feeder and the hub forward to refill;
- **buffer full** → stop (`buffer full!`).

Refill supervision uses the odometer: the distance moved during a refill is
compared with two limits.

- Moved less than **5 mm** while trying to refill → stop, ERROR `0x52`
  BUFFER_REFILL_NO_MOTION.
- Moved more than **40 mm** without the buffer becoming full → stop, ERROR `0x51`
  BUFFER_REFILL_STALLED.

Runout: while printing, BOX_STATE polls check the current slot inlet. Filament gone →
status `0x50` RUNOUT and the indicator shows runout. The host then performs the
swap; Kalico orders runout groups by remaining percentage.

`0x04` mode 1 disarms the interrupts, stops the hub and returns to IDLE.

## 8. Motor-blocked monitor and detooth

- A periodic RT timer (100 ms, `motor_blocked_detect_timer`) evaluates the hub
  driver's blocked/overload signals (`MS3791_is_blocked`, `MS3791 is overload`) and
  sets the motor-blocked flag used by `pull_back_hub`.
- **Reverse detooth**: after reverse moves a slot feeder can stay engaged; the
  firmware briefly reverses it (`feeder reverse detooth`, timeout 1 s), then
  disables detooth until a one-shot timer re-enables it after **11.1 s**
  (`feeder_reverse_detooth_reenable_timer`). The feature is switchable at runtime
  (MSH `reverse_detooth_set`, K2-OpenHost ID 6).

## 9. Error code summary

| Code | Name | Origin |
| --- | --- | --- |
| `0x05` | STAGE0_SENSOR_TIMEOUT | hub sensor not reached in 7 s |
| `0x06` | BUFFER_FILL_TIMEOUT | buffer not full within 30 s (`cmd_parse_out` path) |
| `0x08` | SLOT_EMPTY | no filament at the inlet (load precondition) |
| `0x09` | STAGE0_ODOMETER_TIMEOUT | no 20 mm of odometer travel in 2 s |
| `0x0A` | FEED_TIMEOUT | 25 s feed without completion, or no movement at stage 2 |
| `0x0B` | OVERTRAVEL | more than 3000 mm fed |
| `0x0C` | ODOMETER_STALLED | odometer still for 500 ms during stage 4 |
| `0x0D` | BUFFER_NOT_FULL | buffer not full 3 s after feeding |
| `0x0E` | STAGE7_NO_MOTION | no `arg` mm in 300 ms |
| `0x13` | UNLOAD_BUFFER_TIMEOUT | buffer not empty in 5 s |
| `0x14` | UNLOAD_HUB_PE_TIMEOUT | hub sensor not clear in 7 s |
| `0x16` | UNLOAD_NO_FILAMENT | selected slot shows no filament |
| `0x17` | UNLOAD_INLET_CLEAR | inlet clear when the retract starts |
| `0x18` | UNLOAD_ALL_EMPTY | mask 0 and nothing loaded |
| `0x19` | UNLOAD_MOTOR_BLOCKED | hub motor blocked during the retract |
| `0x1A` | UNLOAD_ODOMETER_TIMEOUT | retract not finished in 40 s |
| `0x50` | RUNOUT | inlet empty while printing |
| `0x51` | BUFFER_REFILL_STALLED | more than 40 mm moved without the buffer filling |
| `0x52` | BUFFER_REFILL_NO_MOTION | less than 5 mm moved while refilling |
| `0x53` | print feed error | hub stopped, ERROR |
