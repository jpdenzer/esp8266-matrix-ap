/*
 * MAX7219 wiring finder (diagnostic firmware — build with `pio run -e diag`).
 *
 * Cycles through likely wiring combinations by bit-banging the MAX7219
 * protocol, and lights a pattern that identifies the combination:
 *
 *   Step 1..7  : CLK=D5, DIN=D7 (correct)   -> N SOLID lines lit
 *   Step 8..14 : CLK=D7, DIN=D5 (swapped)   -> N DOTTED lines lit
 *
 *   N = index of the CS candidate:  1=D2  2=D1  3=D3  4=D4  5=D8  6=D6  7=D0
 *
 * Each step shows for 4 s, then 1 s blank. The whole cycle repeats (~70 s).
 * If nothing ever lights: check 5V/GND power and that DIN goes to the INPUT
 * end of the module. Progress is also printed on serial at 115200 baud.
 */

#include <Arduino.h>

static const uint8_t MAX_DEVICES = 4;

struct Candidate { uint8_t pin; const char *name; };
static const Candidate CS_CANDIDATES[] = {
  {D2, "D2"}, {D1, "D1"}, {D3, "D3"}, {D4, "D4"},
  {D8, "D8"}, {D6, "D6"}, {D0, "D0"},
};
static const uint8_t NUM_CS = sizeof(CS_CANDIDATES) / sizeof(CS_CANDIDATES[0]);

static uint8_t pinClk, pinDin, pinCs;

static void shiftByte(uint8_t b) {
  for (int8_t i = 7; i >= 0; --i) {
    digitalWrite(pinDin, (b >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(pinClk, HIGH);
    delayMicroseconds(2);
    digitalWrite(pinClk, LOW);
  }
}

// Write the same register/value to every chip in the chain.
static void writeAll(uint8_t reg, uint8_t val) {
  digitalWrite(pinCs, LOW);
  delayMicroseconds(2);
  for (uint8_t d = 0; d < MAX_DEVICES; ++d) {
    shiftByte(reg);
    shiftByte(val);
  }
  digitalWrite(pinCs, HIGH);   // data latched on rising edge
  delayMicroseconds(2);
}

static void allPinsIdle() {
  pinMode(D5, OUTPUT); digitalWrite(D5, LOW);
  pinMode(D7, OUTPUT); digitalWrite(D7, LOW);
  for (const auto &c : CS_CANDIDATES) { pinMode(c.pin, OUTPUT); digitalWrite(c.pin, HIGH); }
}

static void showPattern(uint8_t lines, uint8_t rowBits) {
  writeAll(0x0F, 0x00);         // display test off
  writeAll(0x09, 0x00);         // no BCD decode
  writeAll(0x0B, 0x07);         // scan all 8 digits
  writeAll(0x0A, 0x04);         // intensity
  for (uint8_t r = 1; r <= 8; ++r) writeAll(r, r <= lines ? rowBits : 0x00);
  writeAll(0x0C, 0x01);         // leave shutdown
}

static void blank() {
  for (uint8_t r = 1; r <= 8; ++r) writeAll(r, 0x00);
  writeAll(0x0C, 0x00);         // shutdown
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n\nMAX7219 wiring finder"));
  allPinsIdle();
}

void loop() {
  for (uint8_t swapped = 0; swapped < 2; ++swapped) {
    for (uint8_t i = 0; i < NUM_CS; ++i) {
      allPinsIdle();
      pinClk = swapped ? D7 : D5;
      pinDin = swapped ? D5 : D7;
      pinCs  = CS_CANDIDATES[i].pin;
      uint8_t step = swapped * NUM_CS + i + 1;

      Serial.printf("[step %2u] CS=%s CLK=%s DIN=%s -> %u %s line(s)\n", step,
                    CS_CANDIDATES[i].name, swapped ? "D7" : "D5",
                    swapped ? "D5" : "D7", i + 1, swapped ? "DOTTED" : "SOLID");

      showPattern(i + 1, swapped ? 0x55 : 0xFF);
      delay(4000);
      blank();
      delay(1000);
    }
  }
  Serial.println(F("--- cycle complete, repeating ---"));
}
