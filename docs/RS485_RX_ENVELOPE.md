# CFS RS485 receive envelope (offline evidence, v0)

## Evidence boundary

This is an **independent C11 receive-only implementation** of the
common CFS wire-envelope described in the existing research:

- [K2 OpenHost firmware tools: rs485.py](https://github.com/MzTechnology97/k2-openhost-firmware-tools/blob/main/src/k2fw/rs485.py):
  host-side frame builder and response checks. This link is an
  interoperability reference, not incorporated source.
- [Original host updater reverse engineering](https://github.com/MzTechnology97/k2-openhost-firmware-tools/blob/main/docs/RS485_UPDATE_PROTOCOL.md):
  documents several literal frame examples and a **destructive updater
  flow which OpenCFS intentionally does not implement**.
- [RFID diagnostic envelope research](https://github.com/MzTechnology97/k2-openhost-firmware-tools/blob/main/docs/CFS_RFID_DIAGNOSTICS.md):
  independent application-side documentation of the CRC and envelope.
- [Original CFS MCU ROM framing/CRC source audit](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/33):
  original SHA-pinned v3.13/v3.22 APP, physical Flash base 0x08003000,
  authenticates OEM sync F7, length +3, CRC-8 poly07 and receiver checks.
  Only source-derived facts are used here; no OEM bytes are copied.

**Important:** The common F7/+3/CRC-8 envelope is independently
corroborated by the original CFS MCU ROM (addresses linked above), in
addition to stock host analyses. This does **not** establish the complete
runtime command set, UART/RS485 timing, bus address selection or every
vendor protocol variant. Nothing is transmitted or executed.

## Envelope

The byte layout decoded by `src/rs485_rx.c` is:

| Offset | Description |
| --- | --- |
| 0 | Fixed synchronization byte `F7` |
| 1 | Bus address; no policy imposed by RX parser |
| 2 | Declared length `L`, minimum 3, maximum 255 |
| 3 | One-byte qualifier (request header or response status); raw only |
| 4 | Command / function number; raw only |
| 5… | Zero to 252 payload bytes |
| last | CRC-8 over `L, qualifier, command, payload` |

Total on-wire byte count is **`L + 3`** (up to 258 bytes).
CRC-8 uses polynomial **`0x07`**, initial value **`0x00`**,
MSB-first (no reflection) and final XOR **`0x00`**.
CRC does **not** cover `F7` or the bus address, matching the
documented host implementation. Address authenticity must be
enforced by a later protocol layer.

### Important MCU application-path length constraint

The original manufacturer helper at physical PC **0x0800A95A..0x0800A960**
checks `(declared_length - 3) < 0x62`, which yields a maximum of
**97 payload bytes in that particular checked path**. The generic
receive-only decoder supports the 252-byte wire envelope bound but
**must not** be treated as proof that the original CFS application,
loader or all command handlers accept 252-byte payloads. Exact
command-specific length policies require separate research and tests.

**Read-only examples already documented elsewhere:**

- Stock version-query envelope:
  `f7 01 04 00 f0 00 4c`.
  This example is valid **only as an offline test vector**; the `f0`
  opcode is also used by the stock updater for other, potentially
  destructive payloads. OpenCFS cannot send `f0` at this stage.
- Version/SN query envelope:
  `f7 01 03 ff 14 06`.

Additional synthetic test frames are constructed locally in the
host unit test harness and **not transmitted to physical hardware**.

## Decoder behavior and limits

- Incrementally ingests arbitrary chunks and calls the supplied
  callback **only for complete CRC-valid envelopes**.
- Applies a strict fixed 258-byte buffer bound and uses no malloc, memcpy,
  memmove or external C library symbols in the Cortex-M4 object.
- Ignores unsynchronized bytes; rejects lengths less than three;
  resynchronizes within a corrupt buffered envelope on later `F7`.
- Allows `F7` inside otherwise valid payloads.
- Explicit `ocfs_rs485_rx_timeout_reset` discards an incomplete frame
  after transport silence. A frame declaring a long length can
  temporarily delay recognition of later frames until it completes,
  fails CRC, or the caller signals a timeout.
- Reports byte/drop/error counters for diagnostics, which are
  **software counters** and not safety or physical transport measures.
- Callback payload memory is borrowed and may only be used during
  that call. Decoder is non-reentrant and requires a single owner
  or external synchronization.

The decoder is *not* a bootloader protocol, firmware updater, command
transmitter, motor actuator, RFID transaction coordinator, hardware
watchdog or ISO14443 stack. Incoming commands are returned as opaque
bytes and must later pass an explicit authorization policy.

## Qualification still required

For production use, capture sanitized UART/RS485 frames across
multiple hardware/firmware generations, verify direction/status
semantics, device identity and address allocation, timing,
resynchronization policy, command authorization and response matching.
Test on an isolated and recoverable CFS after the loader and
motor-stop safety gates are closed.

**No OpenCFS firmware artifact is currently authorized for flash.**
