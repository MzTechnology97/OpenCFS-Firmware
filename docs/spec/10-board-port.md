# Board port: GD32F303 peripheral and pin map

Everything a board port needs to drive the CFS controller (`cfs0_050_G32`), as the
stock application `cfs0_000_153` configures it. Pin names use RT-Thread logical IDs
(`port*16 + pin`, `0x00` = PA0), taken from the stock 112-entry GPIO table at
`0x0802AD5C` [res].

Evidence:
- **[code]** — read from the configuration code or tables;
- **[str]** — factory-test strings;
- **[pcb]** — historical PCB tracing confirmed by the operator [res];
- **[fn]** — public PCB notes and logic-analyzer captures of a running CFS in
  [fake-name/cfs-reverse-engineering](https://github.com/fake-name/cfs-reverse-engineering)
  (`PCBs.md`, `probing/*.dsl`); the captures are from an earlier firmware build;
- **[?]** — unverified.

## 1. Memory and boot

| Item | Value |
| --- | --- |
| Application base | `0x08003000`; the loader occupies `0x08000000..0x08002FFF` (`08-loader-and-recovery.md`) |
| Image header | version `+0x200` (12 chars), CRC-16 `+0x20C`, length `+0x20E` |
| Parameter page | `0x0807F800` (2 KiB), the stock "database", see below [code] |
| SRAM | `0x20000000..0x2000FFFF`; stock RW `0x000..0xBC3`, ZI to `0x6EE7`, MSP `0x20006EE8`, heap above |
| RTOS | RT-Thread 4.x/5.0 (FinSH/MSH, ulog, device framework), Keil scatter loading |
| Console | SEGGER RTT (`jlinkRtt`); `uart0` (USART0, 115200) is defined but its pins clash with the inlet sensors [?] |

### Parameter page ("database")

A flash device `gd32_flash` (descriptor `0x08029D2C`: base `0x0807F800`, 1 page
of `0x800` bytes, ops erase `0x08011AA2` / read `0x08011B12` / write `0x08011B7A`)
backs one table of three slots (`0x08029D4C`), mirrored in RAM at `0x20004A30` [code]:

| Offset | Size | Content |
| --- | --- | --- |
| `0x00` | 4 | CRC-16/XMODEM (poly `0x1021`, init 0, MSB first) of bytes `0x08..0x0F`, upper half zero (`0x08011CE8`) |
| `0x04` | 4 | payload length, `8` |
| `0x08` | 4 | **key 5**: version of the firmware that wrote the page, `0x00010503` in 1.5.3 |
| `0x0C` | 1 | **key 6**: box mode, `0` = standard. Read by `0x0C`, written by `0x50` |
| `0x0D` | 3 | padding, covered by the CRC (`FF FF FF` when it comes from erased flash) |

The rest of the page stays erased. Keys 5..199 map to slot `key − 4` of table 0,
keys 200..399 to a second table that this build does not define (`0x08011E48`).

- **Load at start-up** (`0x08011EBA`): read the 16 bytes and check the CRC. If it
  is bad, set key 5 = own version and key 6 = 0, then save. If it is good but key 5
  differs from the own version, rewrite key 5 only (the mode is kept).
- **Save** (`0x08011DF0`): recompute the CRC, write length 8, erase the page,
  program the record word by word (unlock `0x45670123` / `0xCDEF89AB`).
- **Mode 2** is written by a start-up task (`0x08012B16`) after sensor activity on
  all four slots with the buffer held in one state for 3 s; it then resets. In a
  non-standard mode, buffer state 1 held for 60 × 50 ms switches back to standard,
  saves and resets (`0x0800A026`). What mode 2 enables is [?].
- **Cross-check**: the public 2025 live flash dump (app `cfs0_000_108`) holds
  `16 89 00 00 08 00 00 00 08 00 01 00 00 FF FF FF` at `0x0807F800`: CRC `0x8916`
  matches, version `0x00010008` = 1.0.8, mode 0 [res].
- The loader sub-command `06` erases exactly this page (`08-loader-and-recovery.md`).

## 2. RS-485 host link

| Item | Value |
| --- | --- |
| UART | **USART1** (`uart1`), IRQ 38, TX **PA2**, RX **PA3** [code] |
| Format | **230400 8N1** (driver config `0x38400`, `0x100008`) [code] |
| Transceiver | 3PEAK TPT75176H [fn] |
| DE/RE | No firmware-driven GPIO [code]. The transmitter is wired permanently enabled and the bus works in a pseudo open-collector way, with the idle state set by the 300 Ω pull-up/pull-down termination [fn]. This limits the usable baud rate |

## 3. Motors

### Slot feeders (AT8236 / HTD8236, brushed)

Columns follow the motor-record byte order (+4, +5). The PCB tracing [fn] puts
driver **IN1 on input +5** (PD3, PD1, PD15, PD13) and **IN2 on input +4** (PD2,
PD0, PD14, PD12) for M1..M4 = slots A..D. So stock direction 0 drives IN1 low and
IN2 high. Which way the spool turns also depends on the motor wiring, which is
still to be checked before any movement. Current sense goes through an LM358 stage
with gain and offset over 0.5 Ω shunts [fn].

| Slot | Input +4 | Input +5 | Drive | Current sense |
| --- | --- | --- | --- | --- |
| A | PD2 (`0x32`) | PD3 (`0x33`) | GPIO on/off [code] | PC5 / ADC_IN15 [code][pcb] |
| B | PD0 (`0x30`) | PD1 (`0x31`) | GPIO on/off [code] | **PC0 / ADC_IN10** [code][pcb]. The factory-test string `PC1` is a typo (PC1 is the LCD CS) |
| C | PD14 (`0x3E`) | PD15 (`0x3F`) | GPIO on/off [code] | PA1 / ADC_IN1 [code][pcb] |
| D | PD12 (`0x3C`) | PD13 (`0x3D`) | GPIO on/off [code] | PA0 / ADC_IN0 [code][pcb] |

Drive: direction 0 → +4 high / +5 low, direction 1 → the opposite, stop → both
high (brake); see `06-motors-sensors.md` §1. The BSP registers PWM devices `pwm3`,
`pwm4`, `pwm5`, `pwm8` on PD12, PD13, PD15, PD14 (TIMER3 CH0, CH1, CH3, CH2,
driver table in initialized SRAM at `0x20000810`…`0x20000930`), but no motor record
and no code references them. The feeders do not use PWM in this build.

Current sense: device `adc1` = **ADC1** (`0x40012800`, RCU APB2EN bit 10), 12 bit,
3.3 V reference. The BSP ADC table at `0x0802AC6C` holds entries of
`{peripheral, RCU bit, 16 channel pins, device, name}` for `adc0` (ADC0), `adc1`
(ADC1) and `adc2` (ADC2); its channel-pin list maps ch 10 to `0x20` = PC0. Each
channel is sampled one-shot (enable, read, disable) every 20 ms by the feeder
monitor (`0x0800FC80`) [code].

**Motion state is not a safe-idle proof.** `BOX_STATE = IDLE` can coexist with
feeder or hub motion, `blocked` means "commanded but no ripple / no FG", and zero
FG pulses do not prove the hub is de-energized. A release build has to establish
"motors off" from the commanded outputs themselves (feeder input levels, hub PWM,
brake), not from the reported state. Object-only CI cannot qualify this.

### Hub (BLDC driver MS8828; the firmware strings say MS3791 [fn])

| Signal | Pin | Detail |
| --- | --- | --- |
| PWM | PE13 (`pwm1`) | TIMER0 CH2, TIMER0 full remap (AFIO_PCF0 bits 7:6 = `11`), 20 kHz, active low [code] |
| DIR | PE3 (`0x43`) | high = forward [code] |
| BRAKE | PE4 (`0x44`) | high = brake [code] |
| FG | PE11 (`0x4B`) | edge interrupt; pulse counter at `0x20004990`; 6 pulses per revolution [code][res] |
| Fault | – | none: blocked and overload are computed from FG and the PID output [code] |

## 4. Filament path sensors

| Signal | Pins | Detail |
| --- | --- | --- |
| Inlet A..D | PD7, PD6, PD5, PD4 | low = filament [code] |
| Hub path A..D | PE10, PE9, PE8, PE7 | low = filament [code] |
| Buffer full | PD8 (`0x38`) | EXTI, armed while printing [code] |
| Buffer empty | PD9 (`0x39`) | EXTI, starts a refill [code] |

## 5. Buses and devices

| Device | Bus / pins | Detail |
| --- | --- | --- |
| Odometer MT6826S | CS **PB12**, SCK PB13, MISO PB14, MOSI PB15, on the J6 connector [code][str][fn]. Stock bit-bangs the bus (`0x08006E32`) at ≈ 312 kHz, SPI mode 3 [code][fn] | see "MT6826S transactions" below. Multi-turn count × **0.000788641 mm** (≈ 1268 counts/mm; at 15 bits per turn ≈ 25.8 mm per wheel turn [?]) |
| RFID reader 0 / 1 (FM17622) | software I2C: SCL1 PB10 / SDA1 PB11, SCL2 PB8 / SDA2 PB9 [str][fn] | 7-bit address **0x28** on both buses [code][fn]. Pin table at `0x200001D0`: **reader 0 = PB8/PB9 (J19), reader 1 = PB10/PB11 (J8)** [code]. Slots A/B on reader 0, C/D on reader 1 (`05-preload-rfid.md` §4). See "FM17622 driver" below |
| Temperature/humidity AHT30 | software I2C: SCL PC11, SDA PC12 [str] | |
| EEPROM BL24Cxx (24C16, 2 KiB) | software I2C: SCL PE5, SDA PE6 [code][str][fn] | diagnostic records, RFID usage data, motor block info (`read_motor_info eeprom`) [str] |
| Front LCD (TM1621B, HT1621-compatible) [fn] | CS PC1, RD PC2, WR PC3, DATA PC4 [code] | protocol in `06-motors-sensors.md` §5 |
| LCD backlight | **PB0** (`0x10`) [code] | configured by the LCD init; command `0x30` switches it |

### MT6826S transactions

Each transaction has its own CS-low window. First byte = `command << 4 | addr[11:8]`
(read `0x3`, write `0x6`), second byte = `addr[7:0]`, then up to four data bytes in
the same window (`0x0800CC20` read, `0x0800CC66` write) [code]. Seen on a running CFS [fn]:

| When | MOSI | MISO | Meaning |
| --- | --- | --- | --- |
| start-up, 3× | `30 03 00 00` | `FF FF hi lo` | angle reads (prime the filter) |
| start-up | `30 01 00` | `FF FF 00` | read user ID |
| start-up | `60 01 7E` | – | write user ID `0x7E` |
| start-up | `30 01 00` | `FF FF 7E` | read back: communication OK |
| every **20 ms** | `30 03 00 00` | `FF FF hi lo` | angle burst read |

The angle is `((hi << 8) | lo) >> 1`, 15 bits per turn (`0x0800CCFC`) [code]. The
stock poll timer is `rt_timer_create(…, rt_tick_from_millisecond(20))` (`0x0800B722`)
[code], which matches the 20 ms seen in the capture.

### FM17622 driver

Register access (`0x0800BDD4` write, `0x0800BE3A` read) [code]: write
`S 50 reg val P`, read `S 50 reg P S 51 data(NACK) P`, `reg` masked to 6 bits.
Registers follow the MFRC522-compatible map. Register `0x0F` gives access to the
extended registers (`0x40|addr` then `0xC0|data` to write, `0x80|addr` then read
`0x0F`). The normal flow does not use them.

**Start-up**: SoftReset (`0x01` ← `0x0F`), wait 2 ms, `0x01` must read `0x20`.
The box tries this up to 3 times per reader (`0x0800BF74`). The capture shows
`50 01 0F` / `50 01` / `51 → 20` on both buses, ~1.7 ms apart [fn].

**Open a card** (`0x0800C59E`, done before every detection):

1. SoftReset again.
2. Configure (`0x0800C0E4`): `0x0C` |= `0x10`, `0x15` |= `0x40` (100 % ASK),
   `0x12` = `0x13` = 0, `0x27` = `F1`, `0x28` = `3F`, `0x29` = `01`, `0x26` = `58`,
   `0x18` = `84`, `0x36` = `40`.
3. Antenna: `0x14` |= `0x03` (TX1 and TX2 together), then 10 ms. Modes 1 and 2
   (TX1 or TX2 alone) exist in `0x0800C14C` but are never used. Both antennas of a
   reader are always energized together, which is why a neighbor tag shows up as
   a collision.
4. Activate (`0x0800C53C`): each step loads the FIFO after `0x01` ← Idle and
   `0x0A` ← `0x80` (flush), then sets Transceive (`0x01` ← `0x0C`) and
   `0x0D` ← `0x80`, and checks `0x0A` (FIFO level) after a fixed wait:

| Step | CRC (`0x12`/`0x13`) | FIFO | `0x0D` | Wait | Expected |
| --- | --- | --- | --- | --- | --- |
| WUPA | off | `52` | `87` (7 bits) | 1 ms | 2 bytes ATQA |
| Anticollision CL1 | off | `93 20` | `80` | 2 ms | 5 bytes, BCC = XOR of 4 |
| Select CL1 | on (`80`) | `93 70` uid0..3 BCC | `80` | 2 ms | 1 byte SAK |
| Anticollision CL2 | off | `95 20` | `80` | 10 ms | 5 bytes, BCC |
| Select CL2 | on | `95 70` uid4..7 BCC | `80` | 2 ms | 1 byte SAK |

ATQA `04 00` stops after CL1 (4-byte UID); any other ATQA runs the CL2 steps.
Return codes: 0 OK; 1 no WUPA answer; **2 anticollision failed, which the preload
treats as "several tags"** and answers by rotating the neighbor; 3 select failed.
On failure the antenna is switched off.

**Authenticate** (`0x0800C5D8`): Status2 `0x08` ← 0 if a Crypto1 session is open.
CRC on, FIFO `60 block key[6] uid[4]`, command MFAuthent (`0x0E`), `0x0D` ← `80`,
10 ms. Success = Status2 bit 3 (Crypto1On). The box authenticates once per sector:
it caches the last sector, and opening a card resets the cache to `0x40`.

**Read** (`0x0800C824`): block ≤ 63, CRC on, FIFO `30 block`, Transceive, 10 ms,
FIFO level must be 16, then 16 bytes. After a record read the antenna is switched
off (`0x0800D670`).

**Record** (`0x0800D3DE`) [code]:

- MIFARE Classic 1K (ATQA `04 00`, SAK `08`): Key A is derived from the 4-byte UID
  with AES-128, and blocks **4, 5, 6** (table at `0x200001F8`) are decrypted with
  a fixed AES-128 key (S-boxes at `0x0802BD89` / `0x0802BE89`). The 40-character
  record is `dec(b4) ‖ dec(b5) ‖ dec(b6)[0..7]`. The key table is 48 bytes right
  before the S-box (`0x0802BD59` in 1.5.3): four 12-byte entries, word 0 of each
  builds K1 and word 1 builds K2 (`0x08026156`). The algorithm is in
  `05-preload-rfid.md` §7. OpenCFS does not ship the keys:
  `tools/re/extract_tag_keys.py` writes a git-ignored `local/creality_keys.hpp`
  from your own image, and the Creality decoder (`vendor_decoders.hpp`, `docs/spec/11`) uses it when present.
- Other tags (e.g. NTAG with a 7-byte UID): no authentication. Read pages 4 and 8
  (16 bytes each); the record is the first 20 bytes as 40 uppercase hex
  characters.
- The write routine `card_write` (`0x0800C70E`) is never called (`05-preload-rfid.md` §8).
- OpenCFS runs the reader as a tag session for the multi-vendor pipeline
  (`11-multi-vendor-tags.md`): one open per read, decoders in order, reselect after
  a failed authentication. The stock sequences above stay the reference.

## 6. Slot indicators

Two LEDs per slot [code]:

| Slot | Group A | Group B |
| --- | --- | --- |
| A | PB7 | PC6 |
| B | PB4 | PC7 |
| C | PB5 | PC8 |
| D | PB6 | PC9 |

The loader also drives PC6..PC9 at boot. Which group is red and which blue is [?];
group B carries the error patterns. Modes are listed in `06-motors-sensors.md` §5.

## 7. Board identity

PD10 and PD11 (`0x3A`, `0x3B`) are read at start-up by the function that prints
`hardware version: secure encryption chip fm15l013` / `No secure encryption chip`.
They are the **hardware-version straps behind `hw_flag`** (`0x20000000`) [code].
The reference CFS reads `hw_flag = 0` [hw].

## 8. Not yet mapped

- clock tree set by the application (the loader configures the PLL);
- address straps `PIN_ADDRESS_A/B`, `PIN_BG_CTL`, `PIN_IO_POWER` (factory test only);
- FM15L013 interface (absent on the reference board);
- J6 pins 2 and 7, **PE15** and **PB3** [fn]: no direct use in the stock code
  [code]; reserved on the feeder-assembly connector.
