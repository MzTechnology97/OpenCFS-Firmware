# Motors, odometer and supervision

Behavioral specification of the actuator and sensor drivers in `cfs0_000_153`.

## 1. Slot feeder motors (4 × brushed DC, AT8236)

Each motor has a record (`0x20000270 + i*0xAC`, [res PR #29]): a PWM flag (+0),
two driver input pins (+4, +5), optional PWM device names and channels (+8/+0xC,
+0x14/+0x18), the ADC device name and channel (+0x20/+0x24), the last speed (+0xA8),
the direction (+0xAA) and a monitor timer.

The scatter-loaded records (identical in 1.5.3 stock and v3.22) are [code]:

| Slot | Input +4 | Input +5 | PWM flag / devices | Current sense |
| --- | --- | --- | --- | --- |
| A | PD2 (`0x32`) | PD3 (`0x33`) | 0 / none | `adc1` ch 15 = PC5 |
| B | PD0 (`0x30`) | PD1 (`0x31`) | 0 / none | `adc1` ch 10 = PC0 |
| C | PD14 (`0x3E`) | PD15 (`0x3F`) | 0 / none | `adc1` ch 1 = PA1 |
| D | PD12 (`0x3C`) | PD13 (`0x3D`) | 0 / none | `adc1` ch 0 = PA0 |

No code writes the PWM flag or the device names after start-up, so **all four
feeders are driven on/off by GPIO**. Public PCB tracing puts driver IN1 on input
+5 and IN2 on input +4 (`10-board-port.md` §3). Which way the spool turns for
direction 0 also depends on the motor wiring: check it before any feeder movement.

**Drive (`feeder_run(slot, dir, speed)`, `0x08010054`)**
- GPIO records (the only ones used): direction 0 → input +4 high, input +5 low;
  direction ≠ 0 → input +4 low, input +5 high. Any non-zero speed is full on.
- `speed = 0` stops: both inputs high (brake), the monitor timer stops, and the
  per-motor filters and state are cleared.
- The function also has a PWM branch (period 20 µs / 50 kHz, low time
  `20000 − speed·20000/255` ns on one input, the other fully on). Records select it
  only through the PWM flag, which is never set, so it is dead code in this build.
- Starting a motor arms its monitor timer (current/voltage sampling, see §4).
  Direction and speed are remembered per slot.
- `feeder_run_pulse(slot, dir)`: wait 200 ms, run at full speed for 30 ms, stop.
  Used to relieve gear load before reversing.

**Current monitor (per feeder, RT timer every 20 ms while the motor runs)** [code]

1. Sample the motor's current-sense ADC with the scheduler locked:
   `V = raw × 3.3 / 4096` (device `adc1` = ADC1, 12 bit). Slot A uses channel 15, B 10, C 1, D 0.
2. Keep the last 5 samples, a 100 ms window.
3. **Blocked:** max − min of the window **< 0.05 V**. A running brushed motor shows
   commutation ripple, a stalled one does not. This is the per-feeder `blocked`
   flag the 100 ms supervisor reads.
4. **Load band:** on the first full window, store the average as `starting_vol`.
   - upper limit = starting + **0.2 V** (`thrust`, capped at 3.3 V);
   - lower limit = starting − **0.1 V** (`resistance`, floored at 0).

   Each later window classifies the load as above the upper limit, below the lower
   limit, or in between.
5. A PID pulls the measured value back toward the limit that was crossed. Gains:
   - over the upper limit: kp 2, ki 8, kd 0;
   - under the lower limit: kp 6, ki 160, kd 0 (`at8236_pid_set out… b…`).

   The correction goes to the motor's speed callback (`feeder_pid_output`).
6. Status for diagnostics: `feeder_curr_voltage`, `feeder_thrust_voltage`,
   `feeder_resistance_voltage`, and `motor%d, average_vol, starting_vol` in debug mode.

## 2. Hub motor (BLDC, MS3791 driver)

| Signal | Pin | Notes |
| --- | --- | --- |
| DIR | PE3 | high = forward |
| BRAKE | PE4 | high = brake/stop, low = run |
| PWM | PE13 | 20 kHz, active-low duty `(255 − cmd)/255` |
| FG | PE11 | tachometer pulses, interrupt on edge |

- `hub_forward(speed)` / `hub_reverse(speed)`: release the brake, set DIR, apply the speed command.
- `hub_stop()`: set the brake and command 0.
- **Speed loop**: a 40 ms RT timer [code].
  - Measured speed: FG pulses counted since the last tick, `rps = pulses × 25 / 6`.
    That is **6 FG pulses per revolution**.
  - Target: `rps = (command + trim) × 50 / 255`, so 255 means 50 rps (3000 rpm).
  - PID kp **1.0**, ki **0.75**, kd 0 (adjustable with `ms3791_pid_set`). The output
    0..255 becomes the active-low PWM duty.
  - The controller state resets on every stop.
- **Blocked** is computed in software; there is no driver fault pin [code]:
  - with the hub commanded, every 40 ms tick without FG pulses decrements a counter
    that starts at **20**;
  - at 0 (about **0.8 s** without rotation) `blocked()` is true;
  - any rotation re-arms the counter.
- **Overload** is also computed: the last **56** PID outputs (≈ 2.2 s) are kept in a
  ring. When more than **39** of them are saturated at 255 (≈ 71 %), the overload
  flag is set. A stop clears it.

## 3. Hub odometer (MT6826 magnetic angle sensor)

- SPI (CS PB12, SCK PB13, MISO PB14, MOSI PB15), polled by `mt6826_timer`.
- Multi-turn position integrated from the angle, reported as **float mm**
  (`odometer_mm`). Feeding toward the toolhead makes it **decrease**. Algorithms use
  `start − now` as "fed distance".
- `0x0E` sub 0 resets it; sub 1 reads it (big-endian float).

## 4. Supervision

### Motor-blocked monitor (`motor_blocked_detect_timer`, 100 ms)

Every 100 ms it builds a mask from the hub *blocked* flag and the four feeder
*blocked* flags (each from its current monitor).

- Mask zero → reset the blocked counter.
- Blocked for more than **1000 ms** with *reverse detooth* enabled, and not
  already detoothing:
  - mark detoothing;
  - if the hub is blocked and the active slot is known, perform a **reverse
    detooth** on that slot's feeder;
  - start the 11.1 s re-enable timer.
- Blocked for more than **10 s**:
  - store the mask as `blocked`;
  - log motor info (feeder voltages, PID outputs, hub speeds);
  - write a diagnostic record to EEPROM;
  - stop all motors.
- Hub **overload**: without detooth, set the overload flag (used by
  `pull_back_hub` → `0x19`); with detooth enabled, detooth the active slot.

### Reverse detooth (`feeder_reverse_detooth`)

Releases a feeder gear that stays engaged while the hub pulls:

1. stop the hub, then run it for **60 ms** at full speed against its current direction;
2. stop it, wait **200 ms**, then pulse the active slot's feeder for **30 ms** at
   full speed in the hub's direction;
3. restore the hub's previous direction and speed (`hub reverse update`).

The active slot is the first slot whose hub sensor shows filament. Detooth runs at
most once per **11.1 s** window.

## 5. Environment, display, indicators

| Function | Hardware | Behavior |
| --- | --- | --- |
| Temperature/humidity | AHT10/AHT30 (software I2C on PC11/PC12) | Read **once per second by the `lcd` thread**, which stores °C (signed) and %RH for BOX_STATE bytes 0/1. |
| Front display | HT1621-compatible segment driver: CS PC1, RD PC2, WR PC3, DATA PC4 (bidirectional); backlight PB0 | Thread `lcd`, 1 s period: shows temperature, humidity and the box index. In test mode all lines and the backlight toggle every second. Bus: command `100` + 9 bits; write `101` + 6-bit address + 16-bit words LSB first; read `110` + address, one bit per RD falling edge; WR low for 3 µs per bit. Init (`0x0800A4A8`): commands `0x40 0x30 0x00 0x0A 0x02 0x06` (each sent as `1000` + 8 bits), backlight on, show 0/0/0. Digits: 0–1 humidity, 2–3 temperature (−9..99, minus = word `0x0010` in digit 2), 4 box index (0..4, else 0); digit *n* is one 16-bit word at address *n*×4, segment table at `0x08029C0C` = `1323 0300 1132 1330 0311 1231 1233 1300 1333 1331` (0..9), bit `0x2000` always set. |
| Slot indicators | two LEDs per slot: group A PB7/PB4/PB5/PB6, group B PC6..PC9 (`10-board-port.md` §6), thread `led` | Mode per slot set by `led_set_mode` (`0x0B`, keep-alive, events); see the table below. |
| EEPROM | BL24Cxx (PE5/PE6) | Diagnostic records (motor-blocked dumps) and RFID usage data. |
| Parameter database | internal flash pages (`gd32_flash`) | Key/value store with checksum; key 5 = version magic, key 6 = operating mode. Defaults are restored on checksum error. |

### Indicator modes (thread `led`)

| Mode | Pattern | Set by |
| --- | --- | --- |
| 0 | group-B chase across the slots, 500 ms | boot |
| 1 | group-A chase; slots with filament switch to mode 2 | keep-alive with every slot empty |
| 2 | group A on for each slot that has filament | keep-alive with filament present |
| 3 | group-A breathing | result OK (`0x00`) |
| 4 | group-B blink, 500 ms | `0x05`, `0x09`; hub-only sensor state |
| 5 | group B steady | `0x17`, `0x53`; inlet-only state |
| 6 | group-B double blink (200/200/200/1000 ms) | errors `0x0A..0x0E`, `0x13`, `0x19`, `0x1A`, `0x51`, `0x52`; both sensors clear |
| 7 | group-B breathing | `0x08` slot empty, `0x16`, `0x50` runout |
| 8 | group-B breathing on all slots | `0x18` all empty |
| 9 | alternating groups on all slots, 1.2 s | (other subsystem) |
| 10 | single-LED blink at 500 ms, LED chosen by the mode parameter | factory/aging tests |
| 11 | all LEDs off | |
| 12 | all group-A LEDs on, group B off | |
| 13 | all group-B LEDs on, group A off | |

Codes `0x14` and `0x20` choose a mode from the sensors: hub-only → 4, inlet-only → 5,
neither → 6.

In modes 3–7 only the selected slot runs the pattern; every other slot shows its
inlet sensor on group A (on = filament present) with group B off.

### Front display content

Five 7-segment positions. Each is one 16-bit word written at address `position × 4`;
the `lcd` thread refreshes them every second.

| Position | Content |
| --- | --- |
| 0, 1 | humidity, tens and units (0..99) |
| 2, 3 | temperature, tens and units (−9..99; position 2 shows `-` when negative) |
| 4 | box index 0..4 (from `0x51`) |

Segment bits are a `0x1000`, b `0x0100`, c `0x0200`, d `0x0020`, e `0x0002`,
f `0x0001` and g `0x0010`. Bit `0x2000` is an extra symbol, lit together with the
digits in positions 0, 2, 3 and 4. The font covers 0–9, A–F and one more glyph
(`0x1223`).

## 6. Threads and timers summary

| Name | Kind | Period/priority | Role |
| --- | --- | --- | --- |
| `heartbeat` | thread, prio 11, 512 B | – | liveness/LED blink |
| `preload` | thread, prio 8, 4 KiB | 1 ms loop | insertion, pretension, RFID, usage manager |
| `rs485` | thread, prio 8, 3 KiB | blocking RX | protocol receiver and dispatcher (long operations run inline) |
| `lcd` | thread, prio 11, 1 KiB | – | front display |
| `led` | thread, prio 11, 512 B | – | slot indicators |
| `print_check` | thread, prio 12, 1 KiB | – | buffer refill during PRINT |
| `presd` | soft timer | 20 ms | sensor filtering |
| `motor_blocked_detect_timer` | soft timer | 100 ms | blocked/overload supervision |
| `feeder_reverse_detooth_reenable_timer` | one-shot | 11.1 s | detooth re-enable |
| `tim_rotational_speed_detect` | soft timer | 40 ms | hub speed loop |
| `mt6826_timer` | soft timer | – | odometer polling |
| per-feeder monitor | soft timer | – | current/voltage sampling, blocked flag |

The main thread initializes everything, then runs `switch_mode(database key 6)`
and enables read-on-insert. It then sleeps forever.
