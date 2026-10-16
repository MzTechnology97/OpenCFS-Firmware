# Spool insertion, RFID reading and remaining-filament estimate

Behavioral specification of thread `preload` (`thread_preload`, priority 8,
4 KiB stack) and the RFID manager in application `cfs0_000_153`.

## 1. Sensor filtering (timer `presd`, 20 ms)

A periodic 20 ms soft timer filters the raw sensors of every slot with a
"change after N consecutive samples" filter:

| Input | Samples | Effective delay |
| --- | --- | --- |
| inlet sensor | 6 | 120 ms |
| hub sensor | 128 | about 2.5 s |

The filtered inlet level drives the insert/remove detection below. The filtered
hub level is used by pretension and the RFID moves.

## 2. Insert / remove events

On every pass the preload logic compares the filtered inlet state with the last
known one.

**Insertion**
1. The slot is marked present, and the slot event becomes 1 "inserted".
   If the box itself moved that filament (own action flag), no event is raised.
2. If preload is allowed, the slot is appended to a 4-entry FIFO (`0xFF` = free).
   Preload is allowed when the box is IDLE or PRELOAD and no RFID transaction is pending.

**Removal**
1. The slot is marked absent, and the slot event becomes 2 "removed".
2. The slot is dropped from the FIFO.

Pending events are reported to the host through BOX_STATE status `0x30`
(`03-rs485-protocol.md`). After a completed preload the event becomes 3 if the
filament is still there, 2 otherwise.

## 3. Preload state machine

The thread runs one step every 1 ms.

| State | Action |
| --- | --- |
| 0 INIT | Seed state 3 for a slot that was already pending at boot. |
| 1 WAIT | If preload is not allowed: clear the FIFO. Otherwise pop the next slot; when no hub sensor shows filament from another slot, go to 3. |
| 3 PRETENSION | Set the box state to PRELOAD when preload is enabled. Pretension the slot in every case: feeder forward at full speed until the filtered hub sensor detects filament (≤ 10 s), stop, then feeder reverse until it clears (≤ 10 s), stop. With preload disabled go to FINISH, otherwise go to 2. |
| 2 CHECK_HUB | If any slot's filament is in the hub (`hub has filament`): FINISH. Otherwise go to 4. |
| 4 READ | If the inlet stays occupied (no 2 absent samples within 50 ms) or a read was requested for the slot: run **RFID read and margin estimation** (section 5), then refresh the slot indicator. Reached only when preload is enabled (`0x0D` sub 1, default). |
| FINISH | Back to WAIT; set the slot event to "finished" (3 present / 2 absent); box state back to IDLE; sleep 300 ms. |

Timeouts are logged (`pretension hub insert/pullout fil detect timeout`) but do not
raise box errors.

When the secure-element hardware is present, the same thread also runs the RFID
usage manager task (section 6) on every pass.

## 4. RFID hardware model

- Two reader front-ends (FM17622, software I2C at `0x28`), each with two antennas
  that are always energized together (TX1 + TX2). Register sequences are in
  `10-board-port.md` §5. Mapping:

  | Slot | Reader | Antenna | Neighbor slot |
  | --- | --- | --- | --- |
  | A | 0 | 0 | B |
  | B | 0 | 1 | A |
  | C | 1 | 0 | D |
  | D | 1 | 1 | C |

- A tag passes in front of its antenna once per spool revolution.
- Detection returns: no tag, exactly one tag, or **multiple tags**. Multiple tags
  means the neighbor spool's tag is also in the field.
- Two backends, chosen at boot by hardware detection:
  - **legacy**: plain MIFARE Classic. Authenticate (Key A), read the data blocks,
    and keep a 76-byte internal record per slot.
  - **secure**: FM15L013 secure element on the CFS board, on its own bit-banged
    I2C bus. It authenticates tags and keeps a used/life record per tag, keyed
    by the tag UID (`get/dec life`, `write used/unused`). The record lives in the
    secure element, not on the tag (section 8).

## 5. RFID read and margin estimation

Goal: read the tag and measure the spool radius from the filament length per
spool revolution.

1. **Locate the tag**
   - Up to 3 attempts. The box mostly feeds or rewinds the *neighbor* slot so its
     tag leaves the shared field, and makes small moves of the slot itself.
   - If several tags are seen, the neighbor spool is rotated away: 400 ms moves
     with 800 ms pauses, up to 3 times.
   - Then the box feeds the filament up to the hub (`feed_top_to_hub`).
   - If the tag is still not seen: hub forward at speed 160 for 800 ms, then stop
     for 200 ms, up to 3 times.
2. **Measure one revolution**: hub forward at speed 120, watching the tag every 5 ms
   (20 s overall timeout).
   - On the first detection: read the tag UID and note the odometer position.
   - On the next detection: read the UID again.
     - Same UID: note the second position. One revolution is done.
     - Different UID: retract (`pull_back_hub` + clear hub sensor) and restart
       once (`different rfid uid detected, retry`).
   - A tag seen continuously for more than 1 s means "stuck in the field"; travel
     beyond ±1250 mm means no revolution was found. Both end the measurement with an error.
3. **Read the data**: stop the hub, wait 500 ms, then authenticate and read the tag,
   with 3 attempts.
   - Between attempts: reverse the slot for 1 s, then nudge the hub (speed 100,
     up to 3 s) until the tag is in the field again.
   - On success the base record (40 ASCII bytes) and the "high" block are stored
     for the slot.
4. **Park**: stop the motors and retract the filament from the hub (unload phase 1).
5. **Neighbor cleanup**: if the measured length is implausible or the tag was stuck,
   rotate the neighbor spool away and retry the whole procedure (3 attempts in total).
6. **Compute the remaining filament** from `L` = the odometer distance between
   the two detections, i.e. the filament length per spool revolution, which is the
   circumference at the current winding radius.

   - `L` < π·96 mm (301.44) is clamped to the core (`fil_len_per_rotation=%d !!!`)
     and marked unreliable.
   - **linear %** = (L − π·96) / (π·172 − π·96) × 100
   - **area %** = (π·(L/2π)² − π·48²) / (π·(86² − 48²)) × 100. This is the stored
     value ("area_percent").
   - Both are rounded and clamped to 1..100 (`0xFF` if `L` < 0).

   Implied spool geometry: core Ø 96 mm, full spool Ø 172 mm. These are the
   Creality 1 kg spool constants.
7. The area percentage is stored for the slot and reported by command `0x03`.
   With the secure backend it also initializes the usage manager. A failed
   estimate stores `0xFF` and clears the slot record.

## 6. Usage manager (`rfid_mgr_fila_*`)

Per-slot record (100 bytes) kept while a slot is in use:

| Field | Meaning |
| --- | --- |
| `used_mm` | accumulated filament used, mm |
| `total_mm` | spool length from the tag `len` field (3 digits, meters × 1000; Creality tags carry `0330`) |
| `last_odom` | odometer at the last checkpoint |
| `percent` | area % from the estimate |
| mode | 0 idle, 1 printing, 2 retracting |
| tag copy | 76-byte internal tag record |

| Event | Effect |
| --- | --- |
| `0x04` mode 0 (print start) | Store the odometer position; mode = printing. |
| `0x04` mode 1 (print end) | Add the positive odometer delta (bounded) to `used_mm`; mode = idle. |
| unload phase 1 | Store the position; mode = retracting; `used_mm += 15`; mark dirty. |
| estimate done (`stock_remain_init`) | Copy the tag record; `total_mm` from `len`. For tag type 7 (tags with a used counter), reconcile: if the measured % is more than 10 points below the tag's "used" figure (and under 90 %), trust the measurement (+5) and rewrite `used_mm`. |
| manager task, printing | Every **10 mm** of filament fed (`-odometer`) is added to `used_mm`. For **type 4** records (runtime estimate, also used by K2-OpenHost for third-party tags) the reported percentage is recalculated at once: `start_pct − used_mm·100/total_mm`, minimum 1. So `0x03` changes **during the print**. For **type 7** records (Creality tags with a used counter) the used length and the remaining percentage are sent to the **secure element**, not to the tag, and `0x03` changes only after that write succeeds (section 8). |
| manager task, retracting | After an unload the record returns to idle 30 s later. |

**Hardware gate.** In stock 153 the usage manager runs **only on boards with the FM15L013 secure element**. At boot `0x080032A8` reads two strap pins (`0x3A`, `0x3B`): both low means no secure element. It stores the result in a byte flag, and `stock_remain_gate` (`0x080032E2`) returns it. The `preload` loop calls `rfid_mgr_fila_task` only when that flag is 1 (`0x08006684`-`0x0800668C`), so on boards without the chip the **whole task is skipped**, type 4 included, although type 4 never uses the chip. The flag is at `0x20000000` and the boot log prints `hardware version: secure encryption chip fm15l013` or `No secure encryption chip`. On boards without it the CFS reports the percentage measured at insertion and never updates it while printing. That is the behavior seen on the K2-OpenHost reference printer, where Kalico estimates consumption from G-code instead. An independent firmware can enable live accounting on every board (`UsageManager::live_percent` in [`cpp/`](../../cpp/README.md)).

The value reported by `0x03` for a slot:
- `0xFF` while that slot is the one being loaded;
- the stored area % when the slot holds a valid estimate;
- `0` when the remaining-valid check fails for a present tag;
- `0xFF` otherwise.

## 7. Tag record format (host view)

The 40-character ASCII record (`0x02` reply, `rfid_base`):

| Field | Offset | Length |
| --- | --- | --- |
| month | 0 | 1 |
| day | 1 | 2 |
| year | 3 | 2 |
| supplier | 5 | 4 |
| batch | 9 | 2 |
| mat_id | 11 | 6 |
| color | 17 | 7 (`#RRGGBB` style) |
| len | 24 | 4 (meters, `0330`) |
| number (serial) | 28 | 6 |
| reserve | 34 | 6 |

Third-party tag support (Bambu block 14, Key A derivation) is a K2-OpenHost
addition (`0x57` API) and not part of the stock behavior.

## 8. Tag writes in stock 153

- **Legacy MIFARE backend** (boards without the FM15L013): the application **never
  writes tags**. A MIFARE block-write routine exists (`card_write`, `0x0800C70E`), but
  nothing calls it, directly or through a pointer. The console command
  `rfid write <id> base|high` parses its arguments and does nothing.
- **Secure backend** (boards with the FM15L013): the usage manager does not write
  the tag either. For type 7 records `FUN_08015a42` sends APDUs only to the secure
  element (`CLA 0x80`, encrypted payload with a 16-byte MAC, status `0x9000`):
  - `0x75` selects the tag by its 7-byte UID;
  - `0x7F` P1 `0x12` writes the used length ("write used");
  - `0x7F` P1 `0x11` writes the remaining percentage ("write unused");
  - `0x7B` / `0x7D` read / decrement a life counter ("get life" / "dec life"),
    once the used length passes 110 % of the nominal length.

  The transport is the secure element's bit-banged I2C bus (`0x080144A4` and
  siblings, used only by the secure module `0x08014xxx`-`0x08015xxx`). The
  FM176xx reader driver uses its own routines, and the usage path makes no reader
  call. The consumption record is therefore kept by the secure element, bound to
  the tag UID. It acts as an anti-refill counter: the tag itself is unchanged.
- **No stock path writes an RFID tag** in normal operation, on either backend.
- Third-party tags (Bambu, generic MIFARE) are never written. K2-OpenHost's support
  for them is read-only by design.
