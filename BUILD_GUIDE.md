# Smart Chess Board — Build Guide

## System Architecture

```
[Physical Board]          [Arduino Uno/Mega]         [Laptop]
Reed switches ──────────► Row/Col scan matrix ──────► Web Serial API
Resistor ladders ────────► Analog piece ID     ──────► chess-advisor.html
WS2812B LEDs ◄───────────── FastLED library   ◄──────  Stockfish.js WASM
```

---

## 1. Piece Identification

Each piece gets a **unique resistor glued to its base** (or embedded in a small cavity). A reed switch + 10K pull-up on each square forms a voltage divider with the piece's resistor.

| Piece  | Resistor | ADC (approx) |
|--------|----------|--------------|
| Empty  | —        | 1023 (open)  |
| Pawn   | 1K       | 930–1022     |
| Rook   | 2.2K     | 820–929      |
| Knight | 4.7K     | 680–819      |
| Bishop | 10K      | 512–679      |
| Queen  | 22K      | 310–511      |
| King   | 47K      | 176–309      |

**Tip:** After soldering, use the Serial Monitor at 115200 baud and send `SCAN` to print raw ADC values. Adjust the threshold constants in the `.ino` file to match your actual readings.

---

## 2. Reed Switch Matrix

```
        A0    A1    A2    A3    A4    A5    A6    A7
         |     |     |     |     |     |     |     |
D2 ──┤ ├──┤ ├──┤ ├──┤ ├──┤ ├──┤ ├──┤ ├──┤ ├   (Row 8 / rank 8)
D3 ──┤ ├──...                                      (Row 7 / rank 7)
...
D9 ──┤ ├──...                                      (Row 1 / rank 1)
```

- Each reed switch connects the **row select line** to the **column analog input**
- The piece resistor is in series: `Row pin → Reed switch → Piece resistor → GND`
- A **10K pull-up** on each analog input: `A0–A7 → 10K → 5V`

When a row is pulled LOW and a magnet is present (reed closes):
- Voltage at analog pin = `5V × Rpiece / (10K + Rpiece)`

---

## 3. LED Wiring

Use two **8×8 WS2812B panels** (64 LEDs total) or a single 64-LED strip.

```
D10 ──► DIN of first LED panel
5V  ──► +5V (use external 5V/3A supply — not Arduino's regulator!)
GND ──► GND (common with Arduino)
```

**Serpentine wiring pattern** (row 0 L→R, row 1 R→L, etc.) is handled in the firmware's `squareToLEDIndex()` function. If your strip is wired differently, adjust that function.

**LED colors:**
- 🟢 **Green** — pick up this piece (Stockfish "from" square)
- 🔵 **Blue** — place piece here (Stockfish "to" square)
- 🟡 **Yellow flash** — confirms the move you just made

---

## 4. Base Magnet + Resistor Assembly

For each piece:
1. Drill a 6mm hole in the base, ~5mm deep
2. Insert a **neodymium disc magnet** (6×2mm)
3. Insert resistor leads (trim to 3mm), secure with epoxy
4. The resistor leads are the electrical contacts — they rest on spring contacts on the board surface (or you can wire directly through the board)

**Alternative:** Use a PCB under each square with pads that contact the piece base. This is cleaner but more work to fabricate.

---

## 5. Software Setup

### Laptop side
1. Open `chess-advisor.html` in **Chrome or Edge** (Web Serial requires Chromium)
2. Click **"Connect to Arduino"** and select your COM port
3. Stockfish.js loads automatically from CDN — no install needed
4. The board will analyze when it's White's turn and light up the suggestion

### Arduino side
1. Install **FastLED** library (Arduino IDE → Library Manager → "FastLED")
2. Open `chess_board.ino`, adjust pin constants if needed
3. Calibrate ADC thresholds by sending `SCAN` via Serial Monitor
4. Upload to Arduino Uno (or Mega for more analog pins)

---

## 6. Serial Protocol

| Direction | Message | Meaning |
|-----------|---------|---------|
| Arduino → PC | `READY:chess_board_v1` | Startup handshake |
| Arduino → PC | `BOARD:rnbqkbnr/pp...` | Full board FEN (piece placement) |
| Arduino → PC | `MOVE:e2e4` | Physical move detected |
| PC → Arduino | `LED:e2:e4` | Light up from/to squares |
| PC → Arduino | `SCAN` | Request board state |
| PC → Arduino | `RESET_LEDS` | Turn off all LEDs |
| PC → Arduino | `PING` | Health check → `PONG` |

---

## 7. Color Tracking

The firmware tracks **color** (White/Black) by remembering the previous state. On startup it assumes the standard starting position. After each move, the laptop sends back the authoritative board state so the Arduino stays in sync.

This means if you set up a mid-game position, use **"Load FEN"** in the web app to tell it the correct position before connecting.

---

## 8. Optional Enhancements

- **Buzzer on D11**: beep on move detection (add `tone(11, 440, 100)`)
- **OLED display**: show the best move in text on a small screen
- **Battery power**: 18650 + boost converter for wireless play
- **Bluetooth/WiFi**: replace serial with HC-05 or ESP32 for cable-free operation
- **Clock**: add a chess clock to each side using two buttons + display
