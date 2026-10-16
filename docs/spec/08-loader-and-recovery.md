# Loader behavior and recovery

Behavioral specification of the Creality CFS RS-485 loader at `0x08000000..0x08002FFF`,
from the loader in the public 2025 full-flash dump (loader SHA-256 `99cd9145…dadd8`,
flash SHA-256 `58b8b908…c125`). That loader identifies itself as `cfs0_050_G32`,
the same boot version the analysed printer reports (`boot_ver` in
`/usr/share/klipper/fw/cfs/version.json`). The loader installed on the analysed CFS
has **not** been read back, so everything here is "same release, very likely
identical", not proven.

## 1. Start-up

1. Clocks: PLL to the 120 MHz class (SysTick at HCLK/8 = 15 MHz, so a tick is 1 ms).
   GPIO set-up from built-in pin lists (including `PC6,PC7,PC8,PC9`).
   TIMER1 is a 1 s periodic interrupt.
2. RS-485 on **USART1** at **230400 baud** (the divisor is rounded for 230400).
3. Address byte (RAM) = `0xFE` (not assigned). The 12-byte MCU unique ID
   (`0x1FFFF7E8`) is hashed with a CRC-8 to seed the enumeration back-off.
4. **Boot window: up to 30 s**, listening on the bus:
   - `0x0B` with payload `01` (`LOADER_TO_APP`, broadcast `0xFF`) and a valid
     application → jump to the application at once;
   - the host assigns an address (A1/A0 enumeration) → the window ends and the
     loader **stays in update mode**;
   - nothing within 30 s → jump if the application is valid; otherwise **stay in the
     loader indefinitely** and keep serving commands.
5. **Application validity** (`app_valid`):
   - length `L` = u32 at `0x0800320E`, not `0` and not `0xFFFFFFFF`, and greater than `0x211`;
   - CRC-16 (poly `0x8005`, init 0, MSB first) over `0x08003000..+0x20C`, then six
     zero bytes, then `0x08003212..+L`, equal to the u16 at `0x0800320C`.
6. **Jump**: disable the peripheral clocks it used, clear all NVIC enables and
   pending bits, load MSP from `0x08003000`, branch to the reset vector at `0x08003004`.

## 2. Image header (application side)

| Offset | Content |
| --- | --- |
| `0x000` | vector table (MSP, reset, …) |
| `0x200` | 12-character application version, e.g. `cfs0_000_153` |
| `0x20C` | CRC-16 (u16 LE) |
| `0x20E` | image length (u32 LE) |

## 3. Update commands (`0xF0`)

Frames use the normal envelope (`F7 addr len hdr cmd … crc8`) with command `0xF0`.
`payload[0]` is the sub-command. Replies use command `0xF0` and header `0x00`.

| Sub | Name | Behavior | Reply |
| --- | --- | --- | --- |
| `00` | identity | If the application is valid: boot version + `-` + application version, e.g. `cfs0_050_G32-cfs0_000_153` (25 bytes). Otherwise the boot version and zeros. | 25 bytes |
| `01` | update | Ack, then up to 5 tries to receive the 4-byte image length (0 → error `0x1F`). Then 128-byte chunks, each acknowledged. Write retries: 3 per chunk. | `0x75` per accepted step; `0x20` when the image is complete; `0x21` on a write failure; `0x1F` on bad input |
| `02` | start application | Only if the application is valid: ack, wait 3 s, jump. | `0x75`, or `0x1F` if invalid |
| `03` | token | Transfer token: `0xE0` (so chunks are 128 bytes). | `0xE0` |
| `06` | erase parameters | Erase **only the last 2 KiB page `0x0807F800`** (the application's parameter page). | `0x75`, or `0x1F` on failure |
| `07` | unique ID | The 12 MCU unique-ID bytes. | 12 bytes |
| `04`, `05` | – | not implemented (ignored) | – |

Programming details:
- The destination starts at **`0x08003000`** and only advances; the host never
  sends addresses.
- Data is buffered and programmed 256 bytes at a time.
- A 2 KiB page is erased when the write pointer reaches its start, so only the
  pages the new image covers are erased.
- The loader never erases or writes below `0x08003000`. It has **no upper length
  check**: an image longer than `0x0807F800 − 0x08003000` would overwrite the
  parameter page.

## 4. What this means for recovery

The update path in sections 1–3 has been re-checked independently against the original
loader instructions in [k2-cfs-rfid-tools #38](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/38):
F0 dispatch, 128-byte chunks, the 256-byte writer, page erase at 2 KiB boundaries,
base `0x08003000`, sub-command 06 erasing `0x0807F800`, and no write-path comparison
against the parameter page. The recovery below is therefore **plausible but not proven
on a production CFS** until the installed loader is shown to match.

The loader is never touched by an application update. Any application image can
therefore be replaced over RS-485, as long as the CFS can be power-cycled and
the host speaks this protocol within the 30 s boot window.

| Situation | Recovery |
| --- | --- |
| Interrupted or corrupt flash (CRC wrong) | The loader stays in update mode for good; flash a valid image. |
| A valid image that misbehaves (crash, wrong protocol) | Power-cycle the CFS. Within 30 s, enumerate it (A1/A0) so it stays in the loader, then flash. On the K2 the CFS supply can be switched from the T113 (K2-OpenHost MCU-rail power cycle). |
| A valid image that blocks RS-485 forever | Same as above: the application only starts after the loader window. |
| Loader damaged (only by SWD or an app that writes below `0x08003000`) | SWD only. |

Rules for an independent firmware that wants to stay recoverable:

1. Link at `0x08003000` and fill `0x200` (version), `0x20C` (CRC) and `0x20E` (length).
2. Never erase or program flash below `0x08003000`.
3. Keep the image below `0x0807F800` and treat that page as the parameter store.
4. Do not reprogram the option bytes (read protection or write protection) or the loader.

Still unverified: that the installed loader is byte-identical to this one, the
behavior when power fails between two chunks (the CRC check catches it), and the
bank-1 path for parts with more than 512 KiB (`0x0807FFFF` boundary). The bank-1
path does not apply to a 512 KiB part.

## 5. The jump to the application, seen from the image

The handoff at `0x08001304..0x0800135E` puts GPIOA and USART1 through a reset
(`RCU_APB2RST` bit 2, `RCU_APB1RST` bit 17), clears all eight NVIC enable and
pending words, loads MSP from `0x08003000` and branches to the reset vector. It
does **not** stop SysTick: the loader's 1 ms tick (HCLK/8, interrupt on) is
still running, and SysTick is a system exception the NVIC clear does not touch.
TIMER1 is not reset either, but its NVIC line stays disabled. An image must
therefore accept a SysTick exception as soon as it unmasks interrupts, before it
programs its own tick. The OpenCFS image masks interrupts until VTOR points at
its own table and its SysTick handler does nothing until the application exists
(`13-application.md` §4).

## 6. Creality's updater, `mcu_util_485`

`/usr/bin/mcu_util_485` on the T113 (SHA-256 `dcf3a631…84d8`, 38 444 bytes, the
same file on the CM5 research copy), called by `/etc/init.d/mcu_update` as
`mcu_util_485 -i /dev/ttyS5 -d <fw dir> -b 230400 [-c] [-j /tmp/cfs_update.json]`.
Addresses below are in that ARM32 Linux binary [code].

### 6.1 Sequence for one CFS

`mcu_update` power-cycles the boards first. The CFS is therefore in its loader,
inside the 30 s window, when `mcu_util_485` enumerates it, and the assignment
keeps it in the loader (§1). Every step below talks to the **loader**, never to
the application. The application does not need to handle `0xF0`.

| State (`0x1881e..`) | Frame | Notes |
| --- | --- | --- |
| `get_salve_info`, `set_salve_addr` | `A1`, `A0` | enumeration (`03-rs485-protocol.md` §6) |
| `get_version` | `F0 00` | reply `boot-app`, e.g. `cfs0_050_G32-cfs0_000_153`, stored per device at `+0x29` |
| `get_sector_size` | `F0 03` | |
| `erase_flash` | `F0 06` | **CFS only** (`0x15030`: device type 1): the parameter page `0x0807F800` is erased at every update, before the image is sent |
| `update_request` | `F0 01` | payload is the single byte `01` (`0x15198`) |
| `app_len` | 4 bytes | file length from `lseek` (`0x135f0`) |
| `app_data` | chunks | read from the file and sent as they are |
| `update_end` | – | the loader reports the image complete |
| `start_app` | `F0 02` | payload `02` only (`0x1567c`). NACK when the loader finds the image invalid (§1, CRC or length) |

The version string is never sent to the CFS. What the CFS reports afterwards
comes from the 12 bytes at image `+0x200`, read back by the loader.

### 6.2 Which file, and what the file name is used for

- **Directory mode** (no `-j`, `0x13088`): the device's boot version must have
  `_` at positions 4 and 8 (`0x12f88`, else "invalid version"). The files in the
  firmware directory are filtered by name prefix = the 12-character boot version
  (`0x12c2c`; names starting with `.` are skipped, the `.bin` suffix is not
  checked). Exactly one match is required ("we should keep only one firmware
  file"). The first 25 characters of its name become `bin_ver` (`0x13398`).
- **Whether to update** (`0x1366c`): a CFS is updated when its reported version
  is only the boot part (13 characters) or ends in `000_000` (no valid
  application). Otherwise `version.json` decides: for the entry whose `boot_ver`
  matches, update when `atoi(current app version + 9)` is **lower** than
  `atoi(app_ver + 9)`. Only the numeric generation after the second `_` counts.
- **JSON mode** (`-j`, `0x13450`): the entry whose `uuid` matches the device's
  unique ID (`"xx xx …"`) gives the file path. The update is done without any
  version comparison. `bin_ver` is the first 25 characters of the base name, and
  only if it is longer than 25.
- `bin_ver` is used for the log and for `/tmp/.485_mcu_version`. It is not part
  of the protocol.

**What this changes.** The `start_app NACK` of the first custom image on
2026-10-06 came from that image's stale CRC and length
(k2-openhost-t113-bootstrap #11), which is exactly the loader's validity check.
The file name only changed the logged and recorded version
(`cfs0_050_G32-3cf3385dcbc5`); it did not reach the CFS.

### 6.3 Rules for an OpenCFS image

- **Version at `+0x200`:** 12 ASCII characters `cfs0_000_NNN`. The loader stores
  any 12 bytes; the stock updater only reads the digits after the second `_`.
  K2-OpenHost's validator and packager require the `cfsN_NNN_NNN` form and a
  file name `<boot>-<version>[-suffix].bin` that matches it.
- **The generation `NNN` is a policy choice.** On a system that runs stock
  `mcu_update` with a non-empty `version.json` (stock slot A, or a stock OTA), a
  CFS whose generation is lower than the listed one is reflashed with stock
  automatically. With `153`, a stock 153 list leaves OpenCFS in place and a
  newer stock release replaces it. K2-OpenHost slot B keeps that list empty
  (`apply --cfs` only).
- **The parameter page is erased by every update** through this tool (`F0 06`).
  OpenCFS must, and does, treat an erased page as defaults (mode 0).
- **Recovery** needs nothing from the application: power cycle, enumeration
  within 30 s, update. A broken image cannot prevent it, because the application
  only runs after the window.
- **Recovery uses the custom path.** A normal update with the stock list does
  not replace a valid image of the same generation, so a misbehaving
  `cfs0_000_153` image stays until it is flashed explicitly (JSON mode,
  `k2oh-mcu-fw apply --cfs --cfs-image <stock> ...`). Only an invalid image
  (the loader reports `000_000`) is replaced by a normal update.
