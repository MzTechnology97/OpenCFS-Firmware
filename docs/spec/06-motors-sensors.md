# Motors, odometer and supervision

Behavioral specification of the actuator and sensor drivers in `cfs0_000_153`.

## 1. Slot feeder motors (4 × brushed DC, AT8236)

Each motor has a record (`0x20000270 + i*0xAC`, [res PR #29]) with two driver
inputs, a PWM channel per input, the last command, the direction and a monitor timer.

| Slot | IN1 | IN2 | Current sense |
| --- | --- | --- | --- |
| A | PD2 | PD3 | PC5 |
| B | PD0 | PD1 | PC1 |
| C | PD14 | PD15 | PA1 |
| D | PD12 | PD13 | PA0 |

**Drive (`feeder_run(slot, dir, speed)`)**
- PWM period 20 µs (50 kHz).
- One input carries PWM with low time `20000 − speed·20000/255` ns (so speed 255 =
  fully on); the other input stays fully on.
- The direction selects which input carries the PWM. This is slow-decay drive,
  with braking between pulses.
- Records configured without PWM use plain GPIO on/off.
- `speed = 0` stops: both inputs high (brake), the monitor timer stops, and the
  per-motor filters and state are cleared.
- Starting a motor arms its monitor timer (current/voltage sampling, see §4).
  Direction and speed are remembered per slot.
- `feeder_run_pulse(slot, dir)`: wait 200 ms, run at full speed for 30 ms, stop.
  Used to relieve gear load before reversing.

## 2. Hub motor (BLDC, MS3791 driver)

| Signal | Pin | Notes |
| --- | --- | --- |
| DIR | PE3 | high = forward |
| BRAKE | PE4 | high = brake/stop, low = run |
| PWM | PE13 | 20 kHz, active-low duty `(255 − cmd)/255` |
| FG | PE11 | tachometer pulses, interrupt on edge |

- `hub_forward(speed)` / `hub_reverse(speed)`: release the brake, set DIR, apply the speed command.
- `hub_stop()`: set the brake and command 0.
- **Speed loop**: a 40 ms RT timer computes the speed (rps) from the FG pulse
  periods (ring buffer of 56 samples). A PID correction is added to the open-loop
  command (`hub_target_speed`, `hub_curr_speed`, `hub_pid_output`).
  PID gains are configurable (`ms3791_pid_set kp ki kd`). The controller state
  resets on every stop.
- The MS3791 reports **blocked** and **overload** conditions.

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
| Temperature/humidity | AHT10/AHT30 (I2C on PC11/PC12) | Periodically read into the BOX_STATE bytes 0/1 (°C signed, %RH). |
| Front display | segment LCD controller (CS/RD/WR/DAT), thread `lcd` | Shows humidity, box number (`slave_num`) and light on/off. |
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
| 10..13 | further patterns | not decoded yet |

Codes `0x14` and `0x20` choose a mode from the sensors: hub-only → 4, inlet-only → 5,
neither → 6.

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
