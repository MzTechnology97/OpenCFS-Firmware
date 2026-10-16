# Multi-vendor filament tags (OpenCFS design)

This chapter is **not stock behavior**. It describes how OpenCFS extends the
stock reader (`10-board-port.md` §5, `05-preload-rfid.md` §4–§7) to read more
tag formats on the CFS itself, instead of through the K2-OpenHost API7 bridge.

## 1. Rules

- **Read-only.** OpenCFS never writes a tag: no block or page write, no sector
  trailer change, no NTAG password or lock-bit command. The reader driver has
  no write primitive at all.
- **Vendor keys stay with the user.** Creality, Bambu or any other
  manufacturer key or KDF secret is never stored in the repository. Each user
  generates a git-ignored `local/` header on their own PC (§6). Without it the
  matching decoder is not built in, and those tags report only UID and type.
- **No key recovery.** No nested, darkside or dictionary attacks. A MIFARE
  Classic tag whose key is not derivable or configured stays unread.
- **Stock compatibility.** Command `0x02` keeps returning the 40-character
  Creality record for Creality tags. Everything else goes through a new command (§5).

## 2. Why on the CFS

API7 can only lend Key A values to the stock RFID task and capture blocks 4/5.
The tag passes the antenna once per spool revolution, so each vendor attempt
costs a full stock read cycle with spool motion and fixed waits. Chaining vendors
is slow and can leave a capture incomplete (`k2-cfs-rfid-tools`
`docs/third-party-tags.md`).

On the CFS, one tag activation can run every applicable key strategy while the
tag is in the field. A failed MIFARE authentication only needs a new
WUPA/anticollision/select (a few ms), not another revolution.

The split stays format-neutral where it can. The CFS does RF access, key
strategies, raw capture and the few fields the box itself needs. The host keeps
profile and material-library mapping.

| Layer | Responsibility |
| --- | --- |
| Reader driver (board) | FM17622 access, activation, Key A auth, 16-byte READ (Classic blocks or 4 Type-2 pages) |
| Tag pipeline (core) | ordered decoders, per-UID routing cache, one field window per read |
| Decoders (core) | key strategy, blocks/pages to read, normalized fields |
| Host (Kalico) | material/profile mapping, UI, persistence of remaining filament |

## 3. Tag technologies on the FM17622

| Technology | Stock support | OpenCFS |
| --- | --- | --- |
| MIFARE Classic 1K (ATQA `04 00`, SAK `08`) | Creality decryption | Creality, Bambu, configured key lists |
| MIFARE Classic Mini / 4K (SAK `09` / `18`) | not read | key lists, sector 1 (blocks 4..6 sit in sector 1 on every size) |
| NFC Type 2 / NTAG21x / Ultralight | already read: `30 04`, `30 08`, hex record (`0x0800D3DE`) | page reads 4..N, NDEF TLV, open formats |
| NFC Type 4 / ISO-DEP (SAK bit 5: NTAG 424, DESFire with NDEF) | not read | RATS, I-blocks with WTX, SELECT NDEF app / CC / file, READ BINARY (read-only) |
| 10-byte UIDs (cascade level 3) | not selected (stock stops at CL2) | CL1..CL3, following SAK bit 2 |
| ISO15693 / NFC-V (Prusa production tags) | none | needs the FM17622 datasheet to confirm the analog front-end supports it [?] |

The stock firmware already sends READ to non-Classic tags, so the Type 2 path
is proven on the reader and the antennas.

## 4. Decoders

Decoders run in this order. The routing cache sends a known UID straight to the
decoder that read it last (8 entries, RAM only).

| # | Decoder | Matches | Keys | Reads | Normalized fields |
| --- | --- | --- | --- | --- | --- |
| 1 | Creality | Classic 1K | K1/K2 from the user's firmware (`tools/re/extract_tag_keys.py`) | blocks 4–6 | 40-char record, material id, color, length |
| 2 | Bambu | Classic 1K, 4-byte UID | HKDF-SHA256(salt = user's Bambu secret, IKM = UID, info `"RFID-A\0"`, 96 bytes) → 16 Key A | blocks 1, 2, 4, 5, 6, 14 | material (block 2), detail (block 4), RGBA, weight, diameter, temperatures, length |
| 3 | Key list | any Classic | user list (e.g. QIDI factory key) | blocks 4–6 | raw only; QIDI codes from block 4 bytes 0..2 when the entry is tagged QIDI |
| 4 | Type 2 / NDEF | NTAG / Ultralight | none | pages 4.., until the end of the NDEF message or 256 bytes | OpenSpool JSON (`type`, `color_hex`, `brand`, `min_temp`, `max_temp`), otherwise raw |
| 5 | Type 4 / NDEF | ISO-DEP (SAK bit 5) | none | NDEF file up to 192 bytes in 48-byte READ BINARY chunks | OpenSpool JSON fields, raw 16-byte chunks |
| – | Snapmaker U1, Anycubic, ELEGOO, TigerTag, OpenTag3D | | | | planned, need real dumps for tests |

Bambu field layout (from the hardware-validated host decoder `box_rfid_bambu.py`):
block 1 = variant id[8] + material id[8]; block 2 = filament type (ASCII);
block 4 = detailed type (ASCII); block 5 = RGBA[4], weight g (u16 LE @4), diameter
(float LE @8); block 6 = drying temp, drying time, –, bed temp, max hotend,
min hotend (u16 LE); block 14 = length m (u16 LE @4).

**Remaining filament** comes from the revolution measurement, which works with
any tag (`05-preload-rfid.md` §5). The decoder only adds the nominal length or
weight that the usage accounting needs. Without it the Creality default (330 m) applies.

## 5. Host command `0x60` TAG_INFO (OpenCFS extension)

Request: `slot` (0..3). Reply status `0x00` OK, `0x01` invalid slot, `0x08` no readout for the slot yet.
Payload (all multi-byte fields little-endian):

| Off | Size | Field |
| --- | --- | --- |
| 0 | 1 | slot |
| 1 | 1 | decoder id: 0 none, 1 Creality, 2 Bambu, 3 key list, 4 Type 2, 5 Type 4 |
| 2 | 1 | flags: bit0 authenticated, bit1 NDEF found, bit2 truncated |
| 3 | 1 | UID length (4, 7 or 10) |
| 4 | 10 | UID, zero padded |
| 14 | 2 | ATQA |
| 16 | 1 | SAK |
| 17 | 16 | material, ASCII, zero padded |
| 33 | 4 | color RGBA |
| 37 | 2 | weight g (0 = unknown) |
| 39 | 2 | length m (0 = unknown) |
| 41 | 2 | diameter × 100 mm (0 = unknown) |
| 43 | 2 | min nozzle °C (0 = unknown) |
| 45 | 2 | max nozzle °C (0 = unknown) |
| 47 | 1 | vendor hint (QIDI = 1 for key-list entries tagged QIDI) |
| 48 | 1 | raw entry count `n` (≤ 6) |
| 49 | 17·n | raw entries: block or first page, 16 bytes |

It is read-only and never starts motion. It reports the result of the last
preload read for the slot.

## 6. Local keys

| File (git-ignored `cpp/board/k2cfs/local/`) | Made by | Enables |
| --- | --- | --- |
| `creality_keys.hpp` | `tools/re/extract_tag_keys.py IMAGE` (your stock image) | decoder 1 |
| `vendor_keys.hpp` | you, from `vendor_keys.example.hpp` | decoder 2 (Bambu salt), decoder 3 (key list) |

The build detects each header with `__has_include`. CI builds without them and
checks that the decoders compile in their disabled form.

## 7. Timing budget (to measure on hardware)

| Step | Stock | OpenCFS target |
| --- | --- | --- |
| Open (SoftReset + configure + field on) | ~15 ms + 10 ms field settle | only once per read, field kept on |
| WUPA / anticollision / select | fixed 1 + 2 + 2 ms | poll the FIFO level / IRQ flags |
| Auth + READ | fixed 10 + 10 ms | poll; several blocks per authenticated sector |
| Retry after failed auth | next revolution | re-select in the same window |

The numbers in the OpenCFS column are targets, not measurements. The driver has
the ComIrq polling (`Fm17622::set_fast(true)`, RxIRq/IdleIRq/ErrIRq, the stock
wait as upper bound) but it stays **off by default** until it is qualified on
hardware.
