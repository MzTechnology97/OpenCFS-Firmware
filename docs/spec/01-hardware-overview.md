# CFS controller — hardware and firmware overview

Target: Creality CFS (K2 series), board `cfs0_050_G32`, application `cfs0_000_153`
(reported as 1.5.3). Analysed image: K2-OpenHost v3.22 (`40dbaad8…`), which is
stock 153 plus 63 patched bytes and 2468 appended bytes.

Evidence tags used in this document:

- **[str]** — firmware string or `__func__` name (Ghidra export, see `00-sources.md`)
- **[code]** — confirmed in disassembly/decompilation
- **[res]** — earlier K2-OpenHost research (see `00-sources.md`)
- **[host]** — host-side Kalico/Creality code ([kalico-k2pro extras](https://github.com/MzTechnology97/kalico-k2pro/blob/k2-pro-openhost/klippy/extras))
- **[hw]** — measured on the real printer

## 1. Memory map

| Region | Range | Notes |
| --- | --- | --- |
| Loader (Creality, RS-485) | `0x08000000..0x08002FFF` | Seen in the public 2025 full-flash dump (SHA-256 `58b8b908…c125`, not stored here). Its own vector table sits at `0x08000000` with reset `0x080005B9`, and it references the app base `0x08003000` at `0x08000AA4`, `0x08001360` and `0x08001A88` [res]. The dump's app is an older build (`cfs0_000_108`, reset `0x08015B21`). The loader carries the boot version string `cfs0_050_G32`, the same `boot_ver` the analysed printer reports. It is very likely the same loader release, but the installed one has not been read back. |
| Application | `0x08003000..0x0802E7DC` (v3.22) | linked at `0x08003000`, vector table at start [res][code] |
| Build policy stop | `0x0802E800` | A 2 KiB page boundary where the v3.23 prototype ends. It is a conservative K2-OpenHost policy limit, **not** a proven maximum of the loader [res] |
| Parameter page | `0x0807F800` (last 2 KiB page) | 16-byte key/value record with CRC-16; layout in `10-board-port.md` §1 [code] |
| SRAM RW (init data) | `0x20000000..0x20000BC3` | 3012 bytes, Keil scatter-compressed at `0x0802DB6C` [res][code] |
| SRAM ZI | `0x20000BC4..0x20006EE7` | 25380 bytes |
| Main stack top / heap start | `0x20006EE8` | heap start moved to `0x20006F28` by v3.19+ [res] |

### Application image format

The application file is the raw flash image from `0x08003000`. It starts with the
Cortex-M vector table and carries a 6-byte header at file offset `0x20C`
(`0x0800320C`). The header is checked by the loader [res PR #36] and confirmed on
all seven images in `firmware/` [code]:

| Offset | Size | Field |
| --- | --- | --- |
| `0x20C` | 2 | CRC-16, little-endian |
| `0x20E` | 4 | image length in bytes, little-endian (equals the file size) |

The CRC-16 uses polynomial `0x8005`, init `0x0000`, MSB first, no reflection and
no final XOR. It is computed over the **whole file with the 6 header bytes set to
zero**, then stored. This matches Creality's signing tool
(`host_crc16_standalone.c`, archived in the K2-OpenHost research).

Loader facts from the public 2025 dump [res PR #36]:
- initial MSP `0x20010000`;
- code in the lower 12 KiB, with `0x08002400..0x08002FFF` erased;
- the handoff at `0x0800134C..0x0800135E` loads the application's MSP and reset vector from `0x08003000` and branches;
- the CRC routine is at `0x08000A08`.

MCU: GigaDevice GD32F303VET6 candidate (Cortex-M4F, 512 KiB flash, 64 KiB SRAM) [res].
The firmware uses the FPU (`FPU active!` in the fault handler [str]).

Earlier research before 2026-10-12 used `0x08010000` as the base; subtract
`0xD000` from those addresses (for example the stock dispatcher `0x0801C3A0` is
`0x0800F3A0`).

## 2. Software platform

- **RT-Thread** kernel (build Sep 2 2026, `2006 - 2022 Copyright`, so 4.1.x/5.0.x) [str]
  with device framework (pin, pwm, adc, i2c, serial with DMA, hwtimer), `ulog`
  coloured logs (`[E/preloading]`), FinSH/MSH shell on `uart`/SEGGER RTT
  (`jlinkRtt`, `RTTUP`/`RTTDOWN`) [str].
- Keil/ARMCC toolchain (scatter loader, `armlibc.syscalls`) [str].
- Application identity string `MF003 V%d.%d.%d build %s %s` [str].
- Boot log: `main init`, `sn_information=%s`, `hardware version: secure encryption chip fm15l013`
  or `No secure encryption chip` [str].
- Application threads/timers named `heartbeat`, `preload`, `rs485`, `print_check`
  [str, `FUN_080032e8`], plus RT timers `motor_blocked_detect_timer`,
  `feeder_reverse_detooth_reenable_timer`, `tim_rotational_speed_detect`,
  `mt6826_timer` [str].

## 3. Peripherals and pins

All pin assignments below come from the factory-test strings unless noted.

### 3.1 Slot feeder motors (4 × brushed DC, AT8236 H-bridge)

| Slot | PWM 1 | PWM 2 | Current sense ADC |
| --- | --- | --- | --- |
| A (0) | PD2 | PD3 | PC5 (`IDETA_ADC_MCU`) |
| B (1) | PD0 | PD1 | PC1 (`IDETB_ADC_MCU`) |
| C (2) | PD14 | PD15 | PA1 (`IDETC_ADC_MCU`) |
| D (3) | PD12 | PD13 | PA0 (`IDETD_ADC_MCU`) |

- Driver chip AT8236 (`at8236_test`, `at8236_pid_set outkp/outki/outkd/bkp/bki/bkd`) [str].
- Motor records at `0x20000270 + index*0xAC`, pins as above [res, PR #29].
- Voltage-based diagnostics: `feeder_curr_voltage`, `feeder_thrust_voltage`,
  `feeder_resistance_voltage`, `motor max voltage` [str].
- "Detooth" reverse: briefly reverse a feeder to release the gear
  (`feeder reverse detooth`, `reverse_detooth_en`, `detooth_test [id] [dir]`) [str].

### 3.2 Hub motor (BLDC, MS3791 driver)

| Signal | Pin |
| --- | --- |
| FG (speed feedback) | PE11 |
| PWM | PE13 |
| Brake | PE4 |

- Closed-loop speed in rps (`hub_target_speed`, `hub_curr_speed`, `hub_pid_output`,
  `ms3791_pid_set kp ki kd`) [str]; FG pulses counted by `tim_rotational_speed_detect` [str].
- Fault detection: `MS3791_is_blocked`, `MS3791 is overload` [str].

### 3.3 Filament path sensors and buffer

- 8 limit inputs `PIN_LIMIT_1..8` (two per slot, likely inlet and hub
  pass-through sensors) paired with RGB slot LEDs `PIN_RED1..4_CTL`, `PIN_BLUE1..4_CTL` [str].
- Buffer limit switches `BUF_LIMIT_1` = PD8, `BUF_LIMIT_2` = PD9, IRQ driven
  (`init buffer irq`, `buffer full!`, `[%d]buffer free!`) [str].
- Hub odometer: **MT6826** magnetic angle sensor on SPI (CS PB12, SCK PB13, MISO PB14,
  MOSI PB15); position reported in mm (`odo_mag read mm`) [str].
  `fil_len_per_rotation` scales spool rotations for RFID margin estimation [str].

### 3.4 RFID

- Two **FM176xx** reader front-ends (`rfid_init_flag[0]`, `rfid_init_flag[1]`,
  `fm176xx`) on I2C (`SetReg/GetReg: send RFID_ADDR`) [str].
  Buses: `SCL1` PB10 / `SDA1` PB11 and `SCL2` PB8 / `SDA2` PB9 [str].
- MIFARE Classic operations: `ReaderA_Select`, `auth block`, `card_read`,
  `card_write`, FIFO via `JREG_FIFOLEVEL/JREG_FIFODATA` [str].
- Optional **FM15L013** secure element: DES self-test, tag-life counters
  (`get_tag_life_count`, `write used/unused`, `dec life`), `rfid_auth_check`,
  `rfid_detect_tag`, `re-auth` [str].
- Tag record: 40 ASCII bytes `month,day,year,supplier,batch,mat_id,col,len,number,reserve`
  (`rfid_base`) plus a binary `rfid_high` block (`len`, `flag`) [str][host].
- Filament usage bookkeeping: `rfid_mgr_fila_{print,idle,back,save,task}`,
  `used_mm`, `total_mm`, `percent`, `rfid write used=%dmm` [str].

### 3.5 Other

- Temperature/humidity: AHT10/AHT30 (`AHT30_SCL_3V` PC11, `AHT30_SDA_3V` PC12) [str];
  reported in BOX_STATE as signed °C and %RH [host][hw].
- EEPROM: BL24Cxx on PE5 (SCL) / PE6 (SDA) [str]; key/value database with checksum
  (`database_get_value`, `database checksum error, load default value`) [str].
- Front panel LCD: segment driver on `PIN_CS/RD/WR/DAT` (HT1621-style),
  commands `lcd hum`, `lcd slave_num`, `lcd light` [str].
- Address straps `PIN_ADDRESS_A/B`, `PIN_BG_CTL`, `PIN_IO_POWER` [str].
- MCU unique ID (`sys_uid[0..2]`) used as the 12-byte RS-485 UniID [str][host].

## 4. Host link

- RS-485 half duplex, **230400 baud**, shared bus `/dev/ttyS5` on the K2 [hw].
- Frame `F7 | addr | len | status/hdr | cmd | payload… | crc8` with
  `len = payload + 3`, CRC-8 poly 0x07 over `len..payload` [res][host].
- Command and status catalogue: see `03-rs485-protocol.md`.

## 5. Open questions (tracked)

- Exact UART instance and DE/RE pin for RS-485.
- Which limit input is the inlet sensor and which is the hub/outlet sensor per slot.
- Hub odometer resolution (steps per mm) and `fil_len_per_rotation` default.
- Loader behaviour and the role of the last flash page.
