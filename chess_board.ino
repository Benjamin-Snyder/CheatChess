/*
 * Smart Chess Board — Arduino Firmware
 * ======================================
 * Hardware:
 *   - 64 reed switches wired as 8x8 matrix (presence detection)
 *   - Analog resistor ladder per column for piece type ID
 *   - WS2812B LED strip (64 LEDs, row-major order)
 *   - Serial @ 115200 baud to laptop
 *
 * Piece resistor values (measured at A0 with 10K pull-up):
 *   Empty  : no magnet  → reed open
 *   Pawn   : 1K  → ~930
 *   Rook   : 2K2 → ~820
 *   Knight : 4K7 → ~680
 *   Bishop : 10K → ~512
 *   Queen  : 22K → ~310
 *   King   : 47K → ~176
 *
 * Wiring:
 *   Row select   → D2–D9   (OUTPUT, pulled LOW to select)
 *   Col read     → A0–A7   (INPUT, reed switch + resistor to GND, 10K pullup)
 *   WS2812B data → D10
 *   WS2812B power→ 5V (use external supply for >8 LEDs lit)
 */

#include <FastLED.h>

// ── Pin config ──
#define ROW_START_PIN  2   // D2–D9 for rows 0–7
#define LED_PIN        10
#define NUM_LEDS       64
#define LED_BRIGHTNESS 60

CRGB leds[NUM_LEDS];

// ── Piece ADC thresholds (center values) ──
// If reed is open (no piece): analog reads ~1023 (no path to GND)
// Adjust these after measuring your actual resistor divider values
struct PieceThreshold {
  int low, high;
  char pieceWhite, pieceBlack; // determined by position later
};

// ADC ranges for each piece type (tune for your resistors!)
const int THRESH_PAWN_LOW   = 850, THRESH_PAWN_HIGH   = 1023;
const int THRESH_ROOK_LOW   = 720, THRESH_ROOK_HIGH   = 849;
const int THRESH_KNIGHT_LOW = 580, THRESH_KNIGHT_HIGH = 719;
const int THRESH_BISHOP_LOW = 440, THRESH_BISHOP_HIGH = 579;
const int THRESH_QUEEN_LOW  = 240, THRESH_QUEEN_HIGH  = 439;
const int THRESH_KING_LOW   = 100, THRESH_KING_HIGH   = 239;
const int THRESH_EMPTY_LOW  = 0,   THRESH_EMPTY_HIGH  = 99;

// ── Board state ──
// board[row][col]: piece character (standard FEN notation)
// Uppercase = White, Lowercase = Black, ' ' = empty
char board[8][8];
char prevBoard[8][8];

// ── LED command from laptop ──
int ledFromRow = -1, ledFromCol = -1;
int ledToRow   = -1, ledToCol   = -1;

// ── Colors ──
CRGB COLOR_FROM(0, 220, 80);    // green — pick this piece
CRGB COLOR_TO(0, 100, 255);     // blue  — place here
CRGB COLOR_LAST(180, 150, 0);   // amber — last move
CRGB COLOR_OFF(0, 0, 0);

// ── State tracking for move detection ──
// We watch for a piece to be lifted then placed
int liftedRow = -1, liftedCol = -1;
char liftedPiece = ' ';
bool awaitingPlacement = false;

void setup() {
  Serial.begin(115200);

  // Row select pins
  for (int i = 0; i < 8; i++) {
    pinMode(ROW_START_PIN + i, OUTPUT);
    digitalWrite(ROW_START_PIN + i, HIGH); // deselect
  }

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  // Startup animation — sweep green across board
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB(0, 60, 30);
    FastLED.show();
    delay(12);
    leds[i] = COLOR_OFF;
  }
  FastLED.show();

  // Read initial board state
  scanBoard();
  memcpy(prevBoard, board, sizeof(board));
  sendBoardState();

  Serial.println("READY:chess_board_v1");
}

void loop() {
  // Handle incoming serial commands from laptop
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    handleCommand(cmd);
  }

  // Scan board every 50ms
  static unsigned long lastScan = 0;
  if (millis() - lastScan >= 50) {
    lastScan = millis();
    scanBoard();
    detectMoves();
  }

  // Update LEDs
  updateLEDs();
  FastLED.show();
}

// ══════════════════════════════════════
//  BOARD SCANNING
// ══════════════════════════════════════

void scanBoard() {
  for (int row = 0; row < 8; row++) {
    // Pull this row LOW to activate reed switches in that row
    digitalWrite(ROW_START_PIN + row, LOW);
    delayMicroseconds(200); // settle time

    for (int col = 0; col < 8; col++) {
      int val = analogRead(col); // A0–A7
      board[row][col] = identifyPiece(val, row, col);
    }

    digitalWrite(ROW_START_PIN + row, HIGH);
  }
}

char identifyPiece(int adcVal, int row, int col) {
  // Determine piece type from ADC value
  char type = ' ';
  if      (adcVal >= THRESH_PAWN_LOW)   type = 'P';
  else if (adcVal >= THRESH_ROOK_LOW)   type = 'R';
  else if (adcVal >= THRESH_KNIGHT_LOW) type = 'N';
  else if (adcVal >= THRESH_BISHOP_LOW) type = 'B';
  else if (adcVal >= THRESH_QUEEN_LOW)  type = 'Q';
  else if (adcVal >= THRESH_KING_LOW)   type = 'K';
  else return ' '; // empty

  // Determine color: rows 0-1 are Black's starting side, rows 6-7 are White's
  // But mid-game we need to track. Simple heuristic: keep prev board color.
  // On startup, use starting position color.
  char prev = prevBoard[row][col];
  if (prev != ' ') {
    // Keep same color as before (piece hasn't left)
    bool wasLower = (prev >= 'a' && prev <= 'z');
    return wasLower ? (char)(type + 32) : type; // lowercase = black
  }

  // New piece appearing: guess color from board position
  // (on starting setup, rows 0-1 = black, 6-7 = white)
  // In real play you'd track this via the move sequence
  if (row <= 1) return (char)(type + 32); // black
  return type; // white (uppercase)
}

// ══════════════════════════════════════
//  MOVE DETECTION
// ══════════════════════════════════════

void detectMoves() {
  // Find differences between board and prevBoard
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (board[r][c] != prevBoard[r][c]) {
        char curr = board[r][c];
        char prev = prevBoard[r][c];

        // Piece lifted (square became empty)
        if (curr == ' ' && prev != ' ' && !awaitingPlacement) {
          liftedRow = r;
          liftedCol = c;
          liftedPiece = prev;
          awaitingPlacement = true;
        }

        // Piece placed (square became occupied) — this is the destination
        if (curr != ' ' && prev == ' ' && awaitingPlacement) {
          // Build UCI move string
          char from[3], to[3];
          squareName(liftedRow, liftedCol, from);
          squareName(r, c, to);

          char uci[8];
          snprintf(uci, sizeof(uci), "%s%s", from, to);

          // Check for promotion (pawn reaching last rank)
          if ((liftedPiece == 'P' && r == 0) || (liftedPiece == 'p' && r == 7)) {
            strncat(uci, "q", sizeof(uci) - strlen(uci) - 1); // auto-queen
          }

          Serial.print("MOVE:");
          Serial.println(uci);

          // Flash the move squares briefly
          flashSquare(liftedRow, liftedCol, CRGB(255,255,0));
          flashSquare(r, c, CRGB(255,255,0));

          liftedRow = liftedCol = -1;
          liftedPiece = ' ';
          awaitingPlacement = false;

          // Clear LED hints (player acted)
          ledFromRow = ledFromCol = ledToRow = ledToCol = -1;
        }
      }
    }
  }

  memcpy(prevBoard, board, sizeof(board));
}

// ══════════════════════════════════════
//  LED CONTROL
// ══════════════════════════════════════

void updateLEDs() {
  FastLED.clear();

  // "FROM" square — green pulse
  if (ledFromRow >= 0) {
    int idx = squareToLEDIndex(ledFromRow, ledFromCol);
    uint8_t bright = (uint8_t)(128 + 127 * sin(millis() / 400.0));
    leds[idx] = CRGB(0, bright, (uint8_t)(bright * 0.35));
  }

  // "TO" square — blue pulse (offset phase)
  if (ledToRow >= 0) {
    int idx = squareToLEDIndex(ledToRow, ledToCol);
    uint8_t bright = (uint8_t)(128 + 127 * sin(millis() / 400.0 + 3.14159));
    leds[idx] = CRGB(0, (uint8_t)(bright * 0.3), bright);
  }
}

void flashSquare(int row, int col, CRGB color) {
  int idx = squareToLEDIndex(row, col);
  for (int i = 0; i < 4; i++) {
    leds[idx] = color;
    FastLED.show();
    delay(80);
    leds[idx] = COLOR_OFF;
    FastLED.show();
    delay(80);
  }
}

int squareToLEDIndex(int row, int col) {
  // WS2812B strips are often wired in serpentine pattern
  // Even rows: left to right, odd rows: right to left
  if (row % 2 == 0) {
    return row * 8 + col;
  } else {
    return row * 8 + (7 - col);
  }
}

// ══════════════════════════════════════
//  SERIAL COMMAND HANDLER
// ══════════════════════════════════════

void handleCommand(String cmd) {
  // "LED:e2:e4" — highlight squares
  if (cmd.startsWith("LED:")) {
    String fromSq = cmd.substring(4, 6);
    String toSq   = cmd.substring(7, 9);
    ledFromRow = 8 - (fromSq[1] - '0');
    ledFromCol = fromSq[0] - 'a';
    ledToRow   = 8 - (toSq[1] - '0');
    ledToCol   = toSq[0] - 'a';
  }

  // "SCAN" — send board state
  if (cmd == "SCAN") {
    scanBoard();
    sendBoardState();
  }

  // "RESET_LEDS"
  if (cmd == "RESET_LEDS") {
    ledFromRow = ledFromCol = ledToRow = ledToCol = -1;
    FastLED.clear();
    FastLED.show();
  }

  // "PING"
  if (cmd == "PING") {
    Serial.println("PONG");
  }
}

// ══════════════════════════════════════
//  HELPERS
// ══════════════════════════════════════

void squareName(int row, int col, char* out) {
  out[0] = 'a' + col;
  out[1] = '0' + (8 - row);
  out[2] = '\0';
}

void sendBoardState() {
  // Send as FEN piece placement only
  Serial.print("BOARD:");
  for (int r = 0; r < 8; r++) {
    int empty = 0;
    for (int c = 0; c < 8; c++) {
      if (board[r][c] == ' ') {
        empty++;
      } else {
        if (empty) { Serial.print(empty); empty = 0; }
        Serial.print(board[r][c]);
      }
    }
    if (empty) Serial.print(empty);
    if (r < 7) Serial.print('/');
  }
  Serial.println();
}
