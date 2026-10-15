# CFS RS-485 protocol (application mode)

Behavioral specification of the CFS side of the host link, derived from stock
`cfs0_000_153` (identical in v3.22 except the added `0x57` diagnostic command)
and cross-checked with the host implementation in Kalico `box_protocol.py`.

Evidence: dispatcher `rs485_cmd_dispatch` at `0x0800F27C` [code], host decoders [host],
live captures in K2-OpenHost `CFS_VALIDATION.md` [hw].

## 1. Physical layer

- Half-duplex RS-485, 230400 baud 8N1, shared with the printer's other bus devices [hw].
- The CFS answers only when addressed; there is no unsolicited traffic.
  Asynchronous events (slot insert/remove) are reported as the status of the next
  BOX_STATE reply (section 5, `0x0A`).

## 2. Frame

```
F7 | addr | len | hdr | cmd | payload[len-3] | crc8
```

| Field | Meaning |
| --- | --- |
| `F7` | sync byte |
| `addr` | target address (requests) or own address (replies) |
| `len` | `payload length + 3` (counts `hdr`, `cmd`, `crc8`) |
| `hdr` | requests: `0xFF` for box commands, `0x00` for auto-address [host]. Replies: status code |
| `cmd` | command; replies repeat the request command (one exception: `0x52`→`0x53`) |
| `crc8` | CRC-8, poly `0x07`, init `0x00`, no reflection, no final XOR, over `len, hdr, cmd, payload` |

Test vectors: `f7 01 04 00 f0 00 4c`, `f7 01 03 ff 14 06` [res].

### 2.1 Receiver

- Byte state machine: wait `F7` → address → length → header → command → payload → CRC [code].
- Address filter: own address, `0xEA` or `0xEB`; other frames are ignored.
  Auto-address frames (`0xFE`/`0xFF` targets, commands `A0..A3`) are handled by a
  separate module (section 6).
- A declared length below 3 aborts the frame.
- Inter-byte timeout: 120 ms without a byte resets the receiver
  (`rs485 timeout, %d` log).
- After a complete, CRC-valid frame the dispatcher runs in the `rs485` thread;
  it records the command and signals a semaphore used by other tasks to detect host activity.

### 2.2 Special addresses

| Address | Behavior |
| --- | --- |
| own (1..4) | normal request/response |
| `0xEA` | broadcast, executed silently: no reply, including errors |
| `0xEB` | broadcast to all boxes; `0x51` acts only on it; replies to `0xEB` frames carry `0xEB` as address |

### 2.3 Common validation

1. **Length check:** each command has a fixed `len`. On mismatch the box replies
   status `0x01` (INVALID_PARAM) with no payload.
2. **Slot mask check** (commands that take one): `0x01, 0x02, 0x04, 0x08` select
   slot 0..3. `0x00` is accepted only where noted. Anything else → `0x01`.
   The selected slot becomes the box's *current slot*.
3. **Busy check** (manual-motion and mode commands): while a preload or reload
   is running the box replies status `0x03` (BUSY).
4. Unknown commands → status `0x01` (silent for `0xEA`).

## 3. Status codes

| Code | Name | Used by |
| --- | --- | --- |
| `0x00` | OK | all |
| `0x01` | INVALID_PARAM | all (length, mask, sub-command) |
| `0x02` | BAD_CRC | host-defined; not emitted by the dispatcher path analysed so far |
| `0x03` | BUSY | 0x04, 0x06, 0x07, 0x10..0x13; 0x50 (mode unchanged) |
| `0x05..0x1A` | feed/unload stage errors | 0x10, 0x11, 0x12, 0x13; see `04-feeder.md` |
| `0x30` | SLOT_EVENT | 0x0A |
| `0x50` | RUNOUT | 0x0A in PRINT state |
| `0x51`, `0x52` | buffer refill stalled / no motion | 0x0A in PRINT state |
| `0x53` | feed error during print (hub stopped) | 0x0A in PRINT state |

Host meanings of `0x05..0x1A`, `0x50..0x52` are listed in `box_protocol.py`
`STATUS_HINTS` and reproduced in `04-feeder.md`.

## 4. Box state (`BOX_STATE` byte)

| Value | State | Entered by |
| --- | --- | --- |
| 0 | IDLE | power-up, end of operations, `0x04` mode 1 |
| 1 | PRELOAD | filament inserted at a slot inlet (automatic pretension + RFID read) |
| 2 | PRINT | `0x04` mode 0 (buffer tracking active) |
| 3 | RELOAD | refill/reload operations |
| 4 | ERROR | any stage error; error code kept for the BOX_STATE status |
| 5 | TEST | factory/aging modes (`0x5D`, MSH commands) |

## 5. Command reference

`mask` = one-hot slot mask, `slot` = the slot it selects.
Sizes are payload bytes (frame `len` = size + 3).

### Identification and configuration

| Cmd | Request | Reply | Behavior |
| --- | --- | --- | --- |
| `0x01` | – | OK, – | Keep-alive. Also refreshes the slot indicator: "has filament" mode with the first loaded slot, otherwise "empty" mode. |
| `0x0C` | – | 1: mode | Current operating mode (database key 6): 0 = standard, other values = aging/test modes. |
| `0x14` | – | 22: ASCII | `"153"` (application version) followed by the 19-character serial number. |
| `0x15` | – | 16 | Six bytes `(0x10*(i+1)) \| v[i]`, then zeros: per-module status/version nibbles (meaning **unverified**). |
| `0x50` | 1: mode | OK / `0x03` | Same as the current mode → status 3. Otherwise store it (database key 6, persisted), reply OK, reset the MCU after 100 ms so the new mode starts. |
| `0x51` | 1: value | none | Only via `0xEB`; stores a box index used by the front display. |
| `0x52` | – | **cmd 0x53**, 2: `[temp, hum]` | Environment readout (same units as BOX_STATE). |
| `0x53` | 2: `a, b` | none | Stores two display values sent by the host (**semantics unverified**). |
| `0x55` | 1: n | 1: n+1 | Echo test (`0xFF` wraps to 0). |
| `0x56` | – | OK | Reset the MCU after 100 ms. |
| `0x5B` | – | 8 | MCU unique-ID derived words. |
| `0x5C` | 16 | 16 | Secure-element challenge/response (FM15L013 path, **under analysis**). |
| `0x5D` | – | – | Enter the factory PCB test mode (state TEST). |

### Sensors

| Cmd | Request | Reply | Behavior |
| --- | --- | --- | --- |
| `0x05` | – | 1: buffer | Buffer position state (see `04-feeder.md` §buffer). |
| `0x08` | 1: `0`/`1` | 1: mask | `0`: inlet sensors of slots 0..3 (bit set = filament present). `1`: hub-path sensors. |
| `0x0A` | – | 6 (or 4 events) | BOX_STATE, below. |
| `0x0E` | 1: `0`/`1` | `0`: OK – / `1`: 4 | `0`: zero the hub odometer. `1`: odometer position, IEEE-754 float, big-endian, mm. |

**BOX_STATE `0x0A` reply**

- Pending slot events (set by the preload task) take priority. The reply has
  status `0x30` and 4 bytes, one per slot: 0 none, 1 inserted, 2 removed,
  3 re-detected. The pending events are then cleared.
- Otherwise the reply has 6 bytes:

  | Byte | Content |
  | --- | --- |
  | 0 | temperature °C, signed |
  | 1 | relative humidity % |
  | 2 | one-shot event byte (`code \| 0x80` if fresh, else last value) |
  | 3 | box state (section 4) |
  | 4 | hub-path sensor mask (the loaded slot while printing) |
  | 5 | per-slot mask from the RFID/remaining module |

- The status depends on the state:
  - **ERROR:** status = the stored error code.
  - **PRINT:** a runout check runs on the current slot. Filament gone at the
    inlet gives `0x50` and the slot indicator shows runout. A feed failure gives
    `0x53`: the hub motor stops and the box enters ERROR.
  - **Any other state:** status `0x00`.

Older CFS firmware 1.1.3 returns a 4-byte variant
`[temp, hum, event, state]` [hw].

### Motion and modes

| Cmd | Request | Reply | Behavior |
| --- | --- | --- | --- |
| `0x04` | 2: `mask`, mode | OK / INVALID / BUSY | Mode 0: enter PRINT on the slot. Arm buffer tracking; if the buffer reads empty, start refilling at once; start filament-usage accounting. Mode 1: leave PRINT (mask `0` allowed). Stop tracking and the hub, and go back to IDLE accounting. BUSY during PRELOAD/RELOAD. |
| `0x06` | 2: `mask`, sub | OK / INVALID / BUSY | Manual slot feeder motor: 1 = forward at the configured forward speed, 2 = reverse at the configured reverse speed, 0 = stop. |
| `0x07` | 1: sub | OK / INVALID / BUSY | Manual hub motor: 1 = forward, 2 = reverse (configured speeds), 0 = stop. |
| `0x09` | 3: target, dir, value | OK, 1 byte `0` | Set a manual speed: target 0 = feeder, 1 = hub; dir 0 = forward, 1 = reverse. |
| `0x0B` | 2: `mask`, mode | OK | Set the slot indicator mode for the slot. |
| `0x0D` | 2: `mask`, sub | OK / error | RFID/preload control. 0 = disable preload on insertion, 1 = enable (default at boot). Pretension still runs; only the hub check and RFID read are skipped. 2 = forced read of the masked slots, synchronous; it rotates the spool and may take minutes per slot, which is why the host waits 190 s per slot. 3 = forced-read variant (**difference unverified**). |
| `0x0F` | 1: 0/1 | OK / INVALID | Stores a boolean option (meaning **unverified**). |
| `0x10` | 3: `mask`, stage, arg | stage status | LOAD stage machine (`cmd_feed_process_2`), see `04-feeder.md`. |
| `0x11` | 2: `mask`, phase | phase status | UNLOAD: phase 0 empties the buffer, phase 1 retracts the slot to its inlet. Phase 1 also closes the slot's RFID usage accounting. |
| `0x12` | 5: `mask`, mode, speed, dist_hi, dist_lo | status | Feed a given distance (mm, big-endian u16) at a speed (`cmd_feed_process_3`). |
| `0x13` | 1: `mask` | status | Feed from the slot to the hub (`feed_top_to_hub`), after the pretension/precondition check. |
| `0x30` | 1: on | OK | Drive one GPIO (RT pin 16) high for `1`, low otherwise (**which output is unverified**). |

### RFID data

| Cmd | Request | Reply | Behavior |
| --- | --- | --- | --- |
| `0x02` | 1: `mask` | ASCII | For each masked slot: `"<A..D>:<record>;"`. The record is the 40-character Creality string or a placeholder. Host field layout: `month,day(2),year(2),supplier(4),batch(2),mat_id(6),color(7),len(4),number(6),reserve(6)`. |
| `0x03` | 1: `mask` | 4 bytes | Remaining percentage per slot, `0xFF` = unknown/not selected. A slot being loaded right now reports `0xFF`. A slot without a valid record reports `0xFF`; in one sensor case the value is `0`. |

## 6. Auto-addressing (`A0`..`A3`)

A separate module answers enumeration frames. Request header `0x00`. Every reply
carries 14 bytes `[device_type=1, mode, uid[12]]`. The mode is 0 in the
application and 1 in the loader. The UID is derived from the MCU unique ID [code][host].

| Cmd | Target | Request payload | Behavior |
| --- | --- | --- | --- |
| `A1` | `0xFE` | `FE FE` | Discovery. Unaddressed boxes reply after a random delay inside a configured window, so several boxes can answer one broadcast. |
| `A0` | `0xFE` | `addr, uid[12]` | Assign `addr` to the box whose UID matches; it replies from the new address. |
| `A2` | addr | – | Presence check of an assigned box. |
| `A3` | – | – | Address table query, observed on hardware [hw]; payload **to be decoded**. |
| `0x0B` | `0xFF` | `01` | Loader only: leave the loader and start the application [host]. |

## 7. Added by K2-OpenHost (not in stock)

- `0x57` diagnostic API (v3.3+, API7 / runtime config API2), dispatched by the
  appended handler at `0x0802DE38` through the patched `0x0800F3A0` branch.
  Specified in [standalone-implementation.md](https://github.com/MzTechnology97/k2-cfs-rfid-tools/blob/main/docs/standalone-implementation.md) and the k2-cfs-rfid-tools
  `handler-v3_22.S`; it is out of scope for the stock reconstruction.
