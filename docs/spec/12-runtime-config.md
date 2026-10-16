# Runtime configuration (OpenCFS, K2-OpenHost API v2 compatible)

This chapter is **not stock behavior**. OpenCFS implements the K2-OpenHost Runtime
Config API v2, as `box_cfs_runtime.py` speaks it, so Kalico can push tuning values
into CFS RAM after every boot without rebuilding or flashing the firmware.

## 1. Wire format (command `0x57`)

| Sub | Request | Reply payload | Status |
| --- | --- | --- | --- |
| `0x10` INFO | – | `version 2, count 28, descriptor revision 1, features 0x2B` | 0 |
| `0x11` GET | `id` | `id, value u16 LE` | 0, 1 bad id |
| `0x12` SET | `id, value u16 LE` | `id, value u16 LE` (the applied value) | 0, **1** bad id / out of range / read-only, **7** not at rest |
| `0x13` RESET | `id` or `0xFF` (all) | – | 0, 1, 7 |
| `0x14` DESCRIBE | `id` | `type, flags, default, min, max` (u16 LE) | 0, 1 |
| other | | – | 1 (K2-OpenHost-only sub-commands) |

Error replies carry no payload, as `box_cfs_runtime.py` requires. Features: bit 0
GET, bit 1 SET, bit 3 DESCRIBE, bit 5 **MCU motion-interlocked SET**. `0x2B` is
not one of the K2-OpenHost bench markers (`0xF7`, `0xB7`, `0x97`, `0xD7`), so the
host's automatic apply stays enabled. Values live only in RAM; a CFS reset brings
back the defaults, and the host applies its profile again after connecting.
Descriptor flags: `0x01` writable, `0x02` idle only, `0x04` advanced, `0x08` RFID,
`0x10` safety. Types: 0 bool, 1 u8, 2 u16.

## 2. Parameters

IDs, names, defaults and ranges are the `box_cfs_runtime.py` catalogue. Stock
sites are app-base addresses (the K2-OpenHost build scripts use the old
`0x08010000` base: subtract `0xD000`); every site was checked byte for byte
against stock 1.5.3.

| ID | Name | Default | Stock site | OpenCFS |
| --- | --- | --- | --- | --- |
| 0 | feeder_forward_speed | 255 | speed table | **read-only**: feeders are GPIO on/off (`06-motors-sensors.md` §1), a speed has no effect |
| 1 | hub_forward_speed | 100 | speed table | `speeds.hub_forward` |
| 2 | hub_transition_speed | 200 | speed table | `speeds.hub_transition` |
| 3 | hub_insert_speed | 155 | speed table | `speeds.hub_insert` |
| 4 | feeder_reverse_speed | 255 | speed table | **read-only**, as ID 0 |
| 5 | hub_reverse_speed | 80 | speed table | `speeds.hub_reverse` |
| 6 | reverse_detooth_enable | 1 | `is_reverse_detooth` `0x080038AA` | `supervision.reverse_detooth` |
| 7 | hub_transition_wait_ms | 3200 | `0x0800411E` (`rt_thread_mdelay(0xC80)`) | `timing.hub_transition_wait_ms` |
| 8 | insert_sensor_timeout | 700 | `feed_until_hub_sensor` `0x080042D0` | `timing.hub_sensor_timeout_ms` = value × 10 |
| 9 | hub_pullback_timeout_ms | 5000 | `unload_buffer` `0x08004CBC` (`wait_buffer_state(0, 5000)`) | `timing.unload_buffer_timeout_ms` |
| 10 | unload_sensor_timeout | 1000 | `0x08004E1A` in `0x08004DF2` | read-only: **dead code**, nothing calls `0x08004DF2` in stock 1.5.3 |
| 11 | rfid_seek_speed | 160 | `0x08005A9E` | `speeds.rfid_seek` |
| 12 | rfid_measure_speed | 120 | `0x08005AC0` | `speeds.rfid_measure` |
| 13 | rfid_reposition_speed | 100 | `0x08005E8A` | `speeds.rfid_reposition` |
| 14 | rfid_seek_move_ms | 800 | RFID locate | `timing.rfid_seek_move_ms` |
| 15 | rfid_seek_settle_ms | 200 | RFID locate | `timing.rfid_seek_settle_ms` |
| 16 | rfid_post_auth_wait_ms | 1000 | `0x08005E7A` | `timing.rfid_retry_reverse_ms` (reverse between read attempts) |
| 17 | rfid_pre_measure_settle_ms | 400 (catalogue: 10) | `0x08005A60`, `mov.w r0, #0x190` at `0x08005A5C` | `timing.rfid_pre_read_settle_ms`; the stock value is 400 ms |
| 18 | rfid_neighbor_detect_delay_ms | 400 | neighbor nudge `0x0800604A` (`mov.w r7, #0x190`) | `timing.rfid_neighbor_move_ms` |
| 19 | rfid_neighbor_settle_ms | 100 | `0x08006076` (`movs r0, #0x64` before `rt_thread_mdelay`, after `feeder_stop_all`) | `timing.rfid_neighbor_settle_ms` |
| 20 | feed_total_timeout_20ms_ticks | 1250 | stage 4 `0x08025D14` | `timing.feed_total_samples` |
| 21 | feed_recovery_timeout_ms | 3000 | stage 4 recovery | `timing.recovery_timeout_ms` |
| 22 | odometer_stall_20ms_ticks | 25 | stage 4 `0x08025CEE` | `timing.stall_samples` |
| 23 | rfid_neighbor_retry_count | 3 | RFID locate | `timing.rfid_locate_attempts` |
| 24 | reverse_detooth_timeout_ms | 1000 | `0x080038F0` (`< 0x3E9` ms blocked) | `supervision.detooth_after_ms` |
| 25 | buffer_fill_timeout_ms | 30000 | `0x08004E62` in `0x08004DF2` | read-only: **dead code**, as ID 10 |
| 26 | transition_detect_timeout_ms | 300 | `0x080042E6` (`mov.w r8, #0x12C`, 300 polls) in `feed_until_hub_sensor` mode 1 | read-only: **dead code**, the only caller (`feed_top_to_hub` `0x08004326`) passes mode 0, so `beq 0x080042E6` at `0x080042C4` is never taken |
| 27 | load_retry_count | 3 | `0x08005616` | `timing.pretension_cycles`: the number of pretension in/out cycles |

**Units.** `wait_hub_sensor(slot, n, level, debounce)` (`0x0800E85A`) counts
**10 ms polls**, so IDs 8 and 10 are 7 s and 10 s by default, despite their
`_ms` names in the host catalogue. IDs 20 and 22 are 20 ms odometer ticks.

IDs 10 and 25 hook a routine with no caller, and ID 26 a branch no caller takes,
so they had no effect in stock either. A read-only parameter answers GET and DESCRIBE (without the writable flag) and
refuses SET with status 1, so nothing is ever stored without an effect.

## 3. Motion interlock

SET and RESET are applied only while the box is provably at rest, **continuously
for 500 ms**:

1. `BOX_STATE` is IDLE (not preload, print, reload, error or test);
2. no motion scope is open (`Box::motion_active()` false), whatever the state says;
3. every feeder has both driver inputs **commanded high** (brake), read back from
   the GPIO output register, and no feeder `run()` since the previous check;
4. the hub is commanded at speed 0, BRAKE (PE4) commanded high, PWM compare at the
   inactive level, no hub `run()` and no FG pulse since the previous check.

Checks 3 and 4 are sticky on purpose. A blocking motion routine can run between
two interlock polls; any movement in that window restarts the 500 ms.

**Why there is no race.** The dispatcher, the feeder routines, preload, print
refill and the supervisor all run in one cooperative main loop. The only
interrupts (USART RX, FG count, SysTick) never start a motor. A SET is checked
and applied inside one dispatcher call, so no motor can start between the check
and the write. This is the property that the patched stock firmware cannot
establish, where motors are started from several RT-Thread threads, the hub PID
timer callback and MSH commands (see the discussion in k2-cfs-rfid-tools #47).

Physical evidence (FG, feeder current ripple) is an additional gate, not the
proof of rest.
