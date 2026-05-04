# DUPLO 10427 Remote Controller (ESP32)

A physical remote controller for the **LEGO DUPLO Train 10427 (2025 edition)** using an ESP32 microcontroller.

> **⚠️ This project is specifically for the 2025 DUPLO train (set 10427).** Older trains (10874, 10875) use a different BLE protocol and are NOT compatible. For older trains, see [Legoino](https://github.com/corneliusmunz/legoino) or [LDTrainRemote](https://github.com/mav00/LDTrainRemote).

## Features

- **Speed control** — potentiometer with quadratic curve (more precision at low speeds, center = stop)
- **LED color cycling** — button cycles through 11 colors (off, white, green, yellow, light blue, dark blue, purple, purple-pink, light pink, red-pink, red)
- **Horn + emergency stop** — button plays horn; if train is moving, also sends stop and latches until pot returns to center
- **Auto-scan & reconnect** — ESP32 scans for the train on boot and reconnects on disconnect
- **Action brick compatible** — train still responds to track action bricks (red=stop, etc.)
- **Hardware watchdog** — auto-reboots ESP32 if firmware hangs (10s timeout)

## Why This Exists

The 2025 DUPLO train (10427) uses a new TI CC2642R BLE chip that **requires BLE bonding** — a feature not supported by existing libraries like Legoino. Without bonding, the train connects but silently ignores all commands. This was discovered through community debugging in [Legoino issue #90](https://github.com/corneliusmunz/legoino/issues/90) and confirmed by [reverse-engineering efforts on Brick StackExchange](https://bricks.stackexchange.com/questions/18907/functionality-of-new-purple-orange-and-green-duplo-train-action-bricks/18975#18975). This project uses raw NimBLE commands to properly bond with the train and control it.

### Key Protocol Differences (10427 vs older trains)

Based on findings from [Legoino issue #90](https://github.com/corneliusmunz/legoino/issues/90):

| Feature | 10874/10875 (old) | 10427 (2025) |
|---|---|---|
| Hub type ID | 0x20 | 0x21 |
| BLE bonding | Not required | **Required** |
| Motor port | 0x00 | 0x32 |
| Light/Sound port | 0x11 | 0x34 |
| BLE chip | Nordic nRF | TI CC2642R |

## Hardware

### Parts List

| Part | Quantity | Notes |
|---|---|---|
| ESP32 DevKit V1 (30-pin) | 1 | Any ESP32-D0WD-V3 board works |
| 10kΩ potentiometer | 1 | Linear taper (B10K) recommended |
| Tactile push buttons | 2 | Momentary, normally open |
| Breadboard | 1 | Full-size or half-size |
| Jumper wires | ~10 | Male-to-male |
| USB cable | 1 | Micro-USB for ESP32 power + flashing |

### Wiring Diagram

```
ESP32 DevKit V1 (30-pin)
┌─────────────────────────┐
│                         │
│  D34 ←── POT wiper     │   Potentiometer (10kΩ):
│  3V3 ──→ POT left pin  │     Left pin  → 3V3
│  GND ──→ POT right pin │     Right pin → GND
│                         │     Middle pin (wiper) → D34
│  D32 ←── BTN1 ──→ GND  │   Button 1 (Color cycle):
│                         │     One leg → D32, other leg → GND
│  D33 ←── BTN2 ──→ GND  │   Button 2 (Horn / Stop):
│                         │     One leg → D33, other leg → GND
│                         │
│  D2  = onboard LED      │   (no wiring needed)
│                         │
└─────────────────────────┘
```

**Potentiometer wiring detail:**
```
  3V3 ──────┐
            │
       ┌────┴────┐
       │  1  2  3 │  ← 3 pins on potentiometer
       └────┬────┘
            │  │
            │  └──── GND
            │
         D34 (wiper / middle pin)
```

**Button wiring** — no external resistors needed (ESP32 internal pull-ups are used):
```
  D32 ───┤ ├─── GND     (Button 1: color cycle)
  D33 ───┤ ├─── GND     (Button 2: horn / stop)
```

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) — install via VS Code extension or CLI
- USB driver for your ESP32's UART chip (CH340 / CP2102 — usually auto-installed)

### Build & Flash

1. **Clone this repo:**
   ```bash
   git clone https://github.com/stasfeelin/duplo-train.git
   cd duplo-train
   ```

2. **Build:**
   ```bash
   pio run
   ```

3. **Flash to ESP32** (connect via USB):
   ```bash
   pio run -t upload
   ```
   PlatformIO auto-detects the serial port. If it doesn't, specify it:
   ```bash
   pio run -t upload --upload-port /dev/cu.usbserial-110   # macOS
   pio run -t upload --upload-port COM3                     # Windows
   ```

4. **Monitor serial output** (optional, for debugging):
   ```bash
   pio device monitor
   ```

### If Using VS Code

1. Install the **PlatformIO IDE** extension
2. Open this folder as a project
3. Click the **→** (Upload) button in the bottom toolbar
4. Click the **plug** icon to open Serial Monitor

## Usage

1. **Power the ESP32** via USB
2. **Turn on the DUPLO train** (green button on top)
3. **Wait for connection** — the onboard LED (D2) blinks slowly while scanning, blinks fast while connecting, and goes solid when connected
4. **Control the train:**
   - **Potentiometer**: center = stop, turn right = forward, turn left = reverse. Speed follows a quadratic curve — more control at low speeds.
   - **Button 1 (D32)**: press to cycle through LED colors
   - **Button 2 (D33)**: press to honk the horn. If the train is moving, it also stops the train. The stop latches — move the pot back to center before resuming.

### Tips

- The first connection takes a few seconds for BLE bonding. Subsequent connections are faster.
- Action bricks on the track still work — the train stops on a red brick until you move the potentiometer.
- If the train disconnects, the ESP32 automatically rescans and reconnects.
- The speed dead zone (center ±250 ADC counts) prevents the train from creeping when the pot isn't perfectly centered.

## BLE Protocol Reference

For anyone working on 10427 BLE integration, here are the confirmed working commands:

```
Service UUID: 00001623-1212-efde-1623-785feabcd123
Char UUID:    00001624-1212-efde-1623-785feabcd123

Motor speed (port 0x32):
  {0x08, 0x00, 0x81, 0x32, 0x11, 0x51, 0x00, SPEED}
  SPEED: int8_t, -100 (full reverse) to +100 (full forward), 0 = stop

LED color (port 0x34):
  {0x0b, 0x00, 0x81, 0x34, 0x11, 0x51, 0x01, 0x04, 0x01, COLOR, 0x00}
  COLOR: 0x00=off, 0x01=white, 0x07=green, 0x08=yellow, 0x09=light blue,
         0x0a=purple, 0x0b=light pink, 0x0c=red, 0x0d=red-pink,
         0x0e=purple-pink, 0x0f=dark blue

Horn (port 0x34):
  {0x0b, 0x00, 0x81, 0x34, 0x11, 0x51, 0x01, 0x07, 0x01, 0x00, 0x00}

BLE Security (REQUIRED):
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  pClient->secureConnection();  // call after connect()
```

## Acknowledgments

- [Legoino issue #90](https://github.com/corneliusmunz/legoino/issues/90) — community discussion that confirmed 10427 requires bonding
- [rzuehlsd/DUPLO_Train_Remote_Controller](https://github.com/rzuehlsd/DUPLO_Train_Remote_Controller) — inspiration for the physical controller concept
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — the BLE library that makes this possible

## License

MIT
