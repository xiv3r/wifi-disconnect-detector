# Wiring Connection Guide — ESP32 Unified WiFi Manager

Based on the firmware you provided, here's the complete wiring for your ESP32 project.

## Components Required

| Qty | Component |
|-----|-----------|
| 1 | ESP32 dev board (any standard 30/38-pin DevKit) |
| 1 | LED (any color) |
| 1 | 220 Ω – 330 Ω resistor (for LED) |
| 1 | Active buzzer (3.3 V compatible) |
| 1 | Momentary push button (BOOT button is usually already on-board) |

---

## Pin Assignments (from firmware)

| Function | GPIO | Constant in code |
|----------|------|------------------|
| Status LED | **GPIO 1** | `PIN_LED` |
| Buzzer | **GPIO 25** | `PIN_BUZZER` |
| BOOT button | **GPIO 0** | `PIN_BOOT_BUTTON` |

---

## Wiring Diagrams

### 1. Status LED → GPIO 1

> ⚠️ **Important:** GPIO 1 is **U0TXD** (the USB-serial TX pin) on most ESP32 boards. It works as an output, but you will lose serial debug output while the LED is connected. If that's a problem, change `PIN_LED` to a free pin such as **GPIO 2**, **GPIO 4**, or **GPIO 5**.

```
ESP32 GPIO 1 ────[ 220Ω ]────►|──── GND
                              LED
                          (anode to resistor,
                           cathode to GND)
```

- LED **anode** (long leg) → resistor → GPIO 1
- LED **cathode** (short leg, flat side) → GND

### 2. Active Buzzer → GPIO 25

For a **3-pin active buzzer module** (with built-in driver):

```
ESP32 GPIO 25 ──── I/O  (signal)
ESP32 3V3      ──── VCC
ESP32 GND      ──── GND
```

For a **2-pin bare active buzzer** (check it's rated 3.3 V):

```
ESP32 GPIO 25 ────(+) buzzer(-)──── GND
```

> If your buzzer is a passive type or draws more than ~12 mA, drive it through an NPN transistor (e.g., 2N2222 / S8050) with a 1 kΩ base resistor and a flyback diode across the buzzer.

### 3. BOOT Button → GPIO 0

Most ESP32 DevKit boards already have a **BOOT** button wired between **GPIO 0** and **GND** with an internal pull-up. **No extra wiring is needed** — the firmware uses `INPUT_PULLUP` and detects a press when the pin reads LOW.

If you want an external/panel button instead:

```
ESP32 GPIO 0 ────┬──── [push button] ──── GND
                 │
             (internal pull-up
              enabled in code)
```

- One side of button → GPIO 0
- Other side → GND
- **Do not** add an external pull-up to 3.3 V (the code enables the internal one).

---

## Complete Connection Table

| ESP32 Pin | Connects To |
|-----------|-------------|
| GPIO 1 | LED anode via 220 Ω resistor |
| GND | LED cathode, buzzer GND, button GND |
| GPIO 25 | Buzzer signal / + |
| 3V3 | Buzzer VCC (if module) |
| GPIO 0 | BOOT button (usually on-board) → GND |

---

## Behavior Reference (what the wiring does)

| Condition | LED (GPIO 1) | Buzzer (GPIO 25) |
|-----------|--------------|------------------|
| STA connected (or no STA SSID configured) | OFF | OFF |
| STA disconnected (SSID set but not connected) | Blinks every 250 ms | Beeps 150 ms ON / 850 ms OFF |
| BOOT held ≥ 5 s | — | — | Triggers **factory reset** |

---

## Notes & Cautions

1. **GPIO 1 (TX) conflict** — as mentioned, this pin doubles as the serial TX line. If you need Serial Monitor output for debugging, remap `PIN_LED` to another GPIO (e.g., 2, 4, 5, 16, 17).
2. **GPIO 0 strapping pin** — GPIO 0 must be HIGH at boot for normal flash mode. The internal pull-up handles this, but **don't hold the button while powering on** unless you intend to enter download mode.
3. **GPIO 25** — safe general-purpose output, no strapping function. Good choice for the buzzer.
4. **Power** — an active buzzer module typically draws 20–30 mA; the ESP32 3V3 regulator can supply this, but for larger buzzers use an external supply with common ground.
5. **No external pull-ups** are needed for the button — the firmware sets `INPUT_PULLUP` in `setup()`.
