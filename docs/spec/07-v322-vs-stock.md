# v3.22 versus stock 153

Byte comparison of the analysed image (K2-OpenHost v3.22, `40dbaad8…`) with the
original Creality application `cfs0_000_153` (`5b076563…`), both linked at `0x08003000`.

- Shared range: 175,672 bytes, **63 bytes differ** in 17 places.
- Appended: 2,468 bytes at `0x0802DE38` (K2-OpenHost handler, source `handler-v3_22.S`
  in k2-cfs-rfid-tools).
- Initial SRAM image (scatter RW data) is identical in both (SHA-256 `553c4915…930a`).

Everything in `docs/03`–`06` describes the stock code, so it applies to both images,
except the items below.

## Patched sites

| Address | Stock function | Stock bytes → v3.22 | Purpose of the hook |
| --- | --- | --- | --- |
| `0x0800320C` | application header (`0x20C`) | CRC `0x8526`, length 175672 → CRC `0xC36F`, length 178140 | loader header re-signed after appending the handler (see `01` §image format) |
| `0x08004116` | post-stage-0 transition push (`FUN_08004104`) | `mov.w r0,#3200` → `bl handler` | runtime ID 7 `hub_transition_wait_ms` (default 3200) |
| `0x080042CA` | `feed_until_hub_sensor` | `mov.w r1,#700` → `bl handler` | runtime ID 8 `insert_sensor_timeout_ms` (default 700 × 10 ms) |
| `0x08005DDC`, `0x08005E62`, `0x08006380`, `0x08006394` | RFID margin estimation (`FUN_080056ea`) | `bl` → `bl handler` | remaining-percent latch for third-party tags (v3.10–v3.12) and stock-task capture |
| `0x0800D4CC`, `0x0800D52C`, `0x0800D590` | `rfid_init_info_read_fm176xx` | `bl legacy_auth_read` → `bl handler` | temporary Key A override on the legacy (MIFARE) backend, API7 |
| `0x0800EF9E` | `0x03` RFID_REMAINING handler | `bl stock_remain_valid` → `bl handler` | report the estimated remaining % for third-party tags |
| `0x0800F3A0` | `rs485_cmd_dispatch` | `b invalid; b cmd51` → `b.w handler` | route command `0x57` (diagnostics/runtime config) and fall back to the stock `0x51`/invalid paths |
| `0x080155D0`, `0x08015604`, `0x08015638` | secure-backend read (`FUN_08015532`) | `bl alt_auth_read` → `bl handler` | Key A override on the FM15L013 backend (not validated for capture) |
| `0x0801A960` | heap setup (`FUN_0801a924`) | literal `0x20006EE8` → `0x20006F28` | reserve the 64-byte volatile sidecar at `0x20006EE8` (v3.19+) |
| `0x0801D5F2` | application start (`FUN_0801d5ec`) | `bl` → `bl handler` | boot wrapper that clears the sidecar header before the stock init |

## Consequences for the reconstruction

- Stock behavior is unchanged except through those hooks. The C++ implementation
  (`src/`) follows stock defaults; the K2-OpenHost runtime IDs map onto `Config`
  (`Speeds`, `Timing`) and need no hooks.
- The command `0x57` is not part of the stock protocol; to keep compatibility with
  current Kalico (`box_cfs_runtime`, `box_rfid_diag`), a new firmware would have to
  implement it on purpose ([standalone-implementation.md](https://github.com/MzTechnology97/k2-cfs-rfid-tools/blob/main/docs/standalone-implementation.md)).
- `BOX_STATE` can read IDLE while filament is moving (seen on hardware, PR #35). The
  C++ core keeps a separate `Box::motion_active()` flag that every motor path sets.
