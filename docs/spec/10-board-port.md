# Board port: GD32F303 peripheral and pin map

Everything a board port needs to drive the CFS controller (`cfs0_050_G32`), as the
stock application `cfs0_000_153` configures it. Pin names use RT-Thread logical IDs
(`port*16 + pin`, `0x00` = PA0), taken from the stock 112-entry GPIO table at
`0x0802AD5C` [res].

Evidence:
- **[code]** — read from the configuration code or tables;
- **[str]** — factory-test strings;
- **[pcb]** — historical PCB tracing confirmed by the operator [res];
- **[?]** — unverified.

## 1. Memory and boot

| Item | Value |
| --- | --- |
| Application base | `0x08003000`; the loader occupies `0x08000000..0x08002FFF` (`08-loader-and-recovery.md`) |
| Image header | version `+0x200` (12 chars), CRC-16 `+0x20C`, length `+0x20E` |
| Parameter page | `0x0807F800` (2 KiB, key/value database with checksum) |
| SRAM | `0x20000000..0x2000FFFF`; stock RW `0x000..0xBC3`, ZI to `0x6EE7`, MSP `0x20006EE8`, heap above |
| RTOS | RT-Thread 4.x/5.0 (FinSH/MSH, ulog, device framework), Keil scatter loading |
| Console | SEGGER RTT (`jlinkRtt`); `uart0` (USART0, 115200) is defined but its pins clash with the inlet sensors [?] |

## 2. RS-485 host link

| Item | Value |
| --- | --- |
| UART | **USART1** (`uart1`), IRQ 38, TX **PA2**, RX **PA3** [code] |
| Format | **230400 8N1** (driver config `0x38400`, `0x100008`) [code] |
| DE/RE | No firmware-driven GPIO found [code]; the transceiver is presumably auto-direction [?] |

## 3. Motors

### Slot feeders (AT8236, brushed)

Columns follow the motor-record byte order (+4, +5). Which input is electrical
IN1 is not verified on the PCB.

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

### Hub (BLDC, MS3791)

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
| Odometer MT6826 | SPI1: CS **PB12** (software), SCK PB13, MISO PB14, MOSI PB15 [code][str] | multi-turn count × **0.000788641 mm** (≈ 1268 counts/mm; at 15 bits per turn ≈ 25.8 mm per wheel turn [?]). Comms check: write user ID `0x7E`, read it back. |
| RFID reader 0 / 1 (FM176xx) | software I2C: SCL1 PB10 / SDA1 PB11, SCL2 PB8 / SDA2 PB9 [str] | which bus serves which reader [?]; antennas per `05-preload-rfid.md` §4 |
| Temperature/humidity AHT30 | software I2C: SCL PC11, SDA PC12 [str] | |
| EEPROM BL24Cxx | software I2C: SCL PE5, SDA PE6 [code][str] | diagnostic records, RFID usage data |
| Front LCD (HT1621-compatible) | CS PC1, RD PC2, WR PC3, DATA PC4 [code] | protocol in `06-motors-sensors.md` §5 |
| LCD backlight | **PB0** (`0x10`) [code] | configured by the LCD init; command `0x30` switches it |

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
- FM15L013 interface (absent on the reference board).
