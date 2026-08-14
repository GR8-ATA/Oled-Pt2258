/*
 * ============================================================================
 *  Digital 5.1 Channel Volume Controller
 *  MCU:        Arduino Nano (ATmega328P)
 *  Volume IC:  PT2258 (6-channel electronic volume controller, I2C)
 *  Display:    SSD1306 128x64 OLED (I2C)
 *  Input:      5 push buttons, IR remote (NEC), 5-stage voltage divider (A0)
 *  Output:     Power relay
 *
 *  Features
 *   - Independent per-channel attenuation (0..79 dB) for all 6 channels
 *   - Master volume (global attenuation) via PT2258 master register
 *   - Channel select cycles: MASTER -> FL -> FR -> CENTER -> SUB -> RL -> RR
 *   - Mute / Unmute (all channels)
 *   - Power button toggles the output relay + soft state
 *   - IR remote control (master + each channel) - NEC protocol
 *   - Serial "learning mode" to capture your own remote's raw hex codes
 *   - 5-stage voltage divider on A0 -> shows a text label (source/mode)
 *   - Settings persisted to EEPROM (restored on power up)
 *
 *  I2C addresses
 *   - PT2258 : 0x44 (7-bit)  == 0x88 write / 0x89 read  (CODE1/CODE2 low)
 *   - SSD1306: 0x3C (7-bit)
 *
 *  Libraries (install via Arduino Library Manager)
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *   - IRremote  (v4.x by Armin Joachimsmeyer)
 * ============================================================================
 */

#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---- IRremote v4.x -----------------------------------------------------------
#define DECODE_NEC          // keep NEC support
#define DECODE_SAMSUNG      // Samsung TV remotes use the SAMSUNG protocol
#define RAW_BUFFER_LENGTH 80  // smaller IR raw buffer -> saves RAM on the Nano
#include <IRremote.hpp>

// ============================================================================
//  PIN MAP  (Arduino Nano)
// ============================================================================
//  I2C: A4 = SDA, A5 = SCL  (shared by PT2258 + OLED)
#define PIN_IR_RECV      4   // IR receiver signal (TSOP1738 / VS1838B) -> D4
#define PIN_BTN_VOL_UP   3
#define PIN_BTN_VOL_DN   2   // moved to D2 (D4 is now used by the IR receiver)
#define PIN_BTN_CH_SEL   5
#define PIN_BTN_MUTE     6
#define PIN_BTN_POWER    7
#define PIN_RELAY        8   // output relay (active HIGH by default)
#define PIN_VDIV         A0  // 5-stage voltage divider input

// Rotary encoder (with push switch) -- optional, works alongside the buttons
#define PIN_ENC_CLK      9   // encoder A / CLK
#define PIN_ENC_DT       10  // encoder B / DT
#define PIN_ENC_SW       11  // encoder push switch -> GND

#define RELAY_ACTIVE_HIGH  true   // set false if your relay board is active LOW

// Auto standby: power off the relay after this many minutes of no activity.
// Set to 0 to disable auto standby.
#define STANDBY_MINUTES    30

// ============================================================================
//  OLED
// ============================================================================
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDR      0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================================
//  PT2258 - register command bases (datasheet)
//  Attenuation = tens_register(0..7) + units_register(0..9), total 0..79 dB
//  0 dB  = loudest,  79 dB = quietest
// ============================================================================
#define PT2258_ADDR        0x44   // 7-bit (0x88 write)

#define PT2258_CLEAR       0xC0   // clear register (must be sent once at start)
#define PT2258_MUTE        0xF8   // +0 = unmute all, +1 = mute all

// Per-channel {10dB step base, 1dB step base}
// Channel order per datasheet: CH1..CH6
static const uint8_t PT_CH_10DB[6] = { 0x80, 0x40, 0x00, 0x20, 0x60, 0xA0 };
static const uint8_t PT_CH_1DB [6] = { 0x90, 0x50, 0x10, 0x30, 0x70, 0xB0 };

// Master
#define PT2258_MASTER_10DB 0xD0
#define PT2258_MASTER_1DB  0xE0

// ============================================================================
//  Channel model
//  We map the 6 PT2258 channels to a 5.1 speaker layout.
//  index 0..5 -> physical channels ; "MASTER" is a 7th virtual selection.
// ============================================================================
enum { CH_FL = 0, CH_FR, CH_CENTER, CH_SUB, CH_RL, CH_RR, CH_COUNT };
#define SEL_MASTER   CH_COUNT      // 6 == master
#define SEL_COUNT    (CH_COUNT + 1)

// Channel names in flash (PROGMEM) to save RAM
const char N_FL[]  PROGMEM = "FRONT L";
const char N_FR[]  PROGMEM = "FRONT R";
const char N_CEN[] PROGMEM = "CENTER";
const char N_SUB[] PROGMEM = "SUB";
const char N_RL[]  PROGMEM = "REAR L";
const char N_RR[]  PROGMEM = "REAR R";
const char N_MST[] PROGMEM = "MASTER";
const char* const CH_NAME[SEL_COUNT] PROGMEM = {
  N_FL, N_FR, N_CEN, N_SUB, N_RL, N_RR, N_MST
};

// ============================================================================
//  TONE PRESETS
//  Each preset sets all 6 channel attenuations at once (0..79 dB, 0 = loudest).
//  Order matches: FL, FR, CENTER, SUB, RL, RR
// ============================================================================
enum { PRESET_FLAT = 0, PRESET_MOVIE, PRESET_MUSIC, PRESET_NIGHT, PRESET_COUNT };

const char P_FLAT[]  PROGMEM = "FLAT";
const char P_MOVIE[] PROGMEM = "MOVIE";
const char P_MUSIC[] PROGMEM = "MUSIC";
const char P_NIGHT[] PROGMEM = "NIGHT";
const char* const PRESET_NAME[PRESET_COUNT] PROGMEM = {
  P_FLAT, P_MOVIE, P_MUSIC, P_NIGHT
};

const uint8_t PRESET_ATTEN[PRESET_COUNT][CH_COUNT] = {
  //  FL  FR  CEN SUB  RL  RR
  {  20, 20, 20, 20, 20, 20 },   // FLAT  - everything level
  {  18, 18, 10,  8, 22, 22 },   // MOVIE - loud center + sub, softer surrounds
  {  12, 12, 24, 16, 20, 20 },   // MUSIC - fronts forward, center pulled back
  {  30, 30, 24, 45, 34, 34 }    // NIGHT - low sub, gentle overall level
};


// ============================================================================
//  5-STAGE VOLTAGE DIVIDER -> text labels  (EDIT THESE to your needs)
//  Reads A0 (0..1023). Five roughly-even bands are decoded to a label.
//  Example use: input source selector, listening mode, etc.
// ============================================================================
const char V_AUX[] PROGMEM = "AUX";
const char V_BT[]  PROGMEM = "BLUETOOTH";
const char V_USB[] PROGMEM = "USB";
const char V_OPT[] PROGMEM = "OPTICAL";
const char V_COX[] PROGMEM = "COAXIAL";
const char* const VDIV_LABELS[5] PROGMEM = {
  V_AUX, V_BT, V_USB, V_OPT, V_COX
};
// ADC thresholds (0..1023). Value below threshold[i] -> band i.
// Defaults assume 5 evenly spaced steps of a resistor ladder.
const int VDIV_THRESHOLD[5] = { 102, 307, 512, 717, 1023 };

// Small RAM buffer for copying PROGMEM strings before printing
char g_strbuf[12];

// ============================================================================
//  Persisted settings (EEPROM)
// ============================================================================
#define EEPROM_MAGIC   0x52        // bump to reset stored config
#define EEPROM_ADDR    0

struct Settings {
  uint8_t magic;
  uint8_t chAtten[CH_COUNT];   // per-channel attenuation 0..79
  uint8_t masterAtten;         // master attenuation 0..79
  uint8_t muted;               // 0/1
  uint8_t powerOn;             // 0/1
  uint8_t preset;              // last applied tone preset index
};
Settings cfg;

// ============================================================================
//  Runtime state
// ============================================================================
uint8_t selection = SEL_MASTER;   // currently selected channel (for +/-)
uint8_t vdivBand  = 0;            // decoded voltage divider band
bool    dirty     = false;        // settings changed -> schedule EEPROM save
unsigned long lastSaveReq = 0;
unsigned long lastActivity = 0;   // for auto standby
bool    oledOk    = false;        // true only if the SSD1306 initialised

// ============================================================================
//  IR CODE MAP  (Samsung TV remote -- SAMSUNG protocol, address 0x0707)
//  These are the STANDARD Samsung TV command codes. If a key on YOUR remote
//  reports a different value, use the Serial "learning mode" (send 'L') to
//  print it, then paste the value below and re-upload.
// ============================================================================
#define IR_POWER      0x02   // POWER
#define IR_MUTE       0x0F   // MUTE
#define IR_VOL_UP     0x07   // VOL +
#define IR_VOL_DN     0x0B   // VOL -
#define IR_CH_NEXT    0x12   // CH ^  (next channel selection)
#define IR_CH_PREV    0x10   // CH v  (prev channel selection)
#define IR_PRESET     0x01   // SOURCE (cycle tone presets) - relearn if needed

// Direct-select each channel (number keys 1..6 ; MASTER = key "0")
#define IR_SEL_FL     0x04   // key "1"
#define IR_SEL_FR     0x05   // key "2"
#define IR_SEL_CENTER 0x06   // key "3"
#define IR_SEL_SUB    0x08   // key "4"
#define IR_SEL_RL     0x09   // key "5"
#define IR_SEL_RR     0x0A   // key "6"
#define IR_SEL_MASTER 0x11   // key "0"

// ============================================================================
//  Button debounce
// ============================================================================
struct Button {
  uint8_t pin;
  bool    lastStable;
  bool    lastReading;
  unsigned long lastChange;
};
Button btnVolUp{PIN_BTN_VOL_UP, HIGH, HIGH, 0};
Button btnVolDn{PIN_BTN_VOL_DN, HIGH, HIGH, 0};
Button btnChSel{PIN_BTN_CH_SEL, HIGH, HIGH, 0};
Button btnMute {PIN_BTN_MUTE,   HIGH, HIGH, 0};
Button btnPower{PIN_BTN_POWER,  HIGH, HIGH, 0};

#define DEBOUNCE_MS   30

// Returns true on a fresh press (falling edge, INPUT_PULLUP => pressed = LOW)
bool buttonPressed(Button &b) {
  bool reading = digitalRead(b.pin);
  unsigned long now = millis();
  if (reading != b.lastReading) {
    b.lastReading = reading;
    b.lastChange  = now;
  }
  if ((now - b.lastChange) > DEBOUNCE_MS && reading != b.lastStable) {
    b.lastStable = reading;
    if (b.lastStable == LOW) return true;   // just pressed
  }
  return false;
}

// Copy a PROGMEM string-table entry into a small RAM buffer for printing
const char* pflash(const char* const table[], uint8_t i) {
  strcpy_P(g_strbuf, (PGM_P)pgm_read_ptr(&table[i]));
  return g_strbuf;
}

// ============================================================================
//  PT2258 low-level helpers
// ============================================================================
bool pt2258Write(uint8_t data) {
  Wire.beginTransmission(PT2258_ADDR);
  Wire.write(data);
  return (Wire.endTransmission() == 0);
}

bool pt2258Write2(uint8_t a, uint8_t b) {
  Wire.beginTransmission(PT2258_ADDR);
  Wire.write(a);
  Wire.write(b);
  return (Wire.endTransmission() == 0);
}

// Set attenuation (0..79 dB) for a physical channel (0..5)
void pt2258SetChannel(uint8_t ch, uint8_t atten) {
  if (ch >= CH_COUNT) return;
  if (atten > 79) atten = 79;
  uint8_t tens  = atten / 10;
  uint8_t units = atten % 10;
  pt2258Write2(PT_CH_10DB[ch] | tens, PT_CH_1DB[ch] | units);
}

// Set master attenuation (0..79 dB)
void pt2258SetMaster(uint8_t atten) {
  if (atten > 79) atten = 79;
  uint8_t tens  = atten / 10;
  uint8_t units = atten % 10;
  pt2258Write2(PT2258_MASTER_10DB | tens, PT2258_MASTER_1DB | units);
}

void pt2258SetMute(bool on) {
  pt2258Write(PT2258_MUTE | (on ? 1 : 0));
}

bool pt2258Init() {
  delay(300);                     // PT2258 needs >=300ms after power-up
  bool ok = pt2258Write(PT2258_CLEAR);
  delay(10);
  return ok;
}

// Push the full current state to the PT2258
void pt2258ApplyAll() {
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) pt2258SetChannel(ch, cfg.chAtten[ch]);
  pt2258SetMaster(cfg.masterAtten);
  pt2258SetMute(cfg.muted);
}

// ============================================================================
//  EEPROM
// ============================================================================
void loadSettings() {
  EEPROM.get(EEPROM_ADDR, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    // First boot / invalid -> sensible defaults
    cfg.magic       = EEPROM_MAGIC;
    for (uint8_t i = 0; i < CH_COUNT; i++) cfg.chAtten[i] = 20;  // -20 dB
    cfg.masterAtten = 30;                                        // -30 dB
    cfg.muted       = 0;
    cfg.powerOn     = 0;
    cfg.preset      = PRESET_FLAT;
  }
}

void requestSave() { dirty = true; lastSaveReq = millis(); }

void noteActivity() { lastActivity = millis(); }

void maybeSave() {
  // Debounced EEPROM write: 2s after the last change (protects flash cycles)
  if (dirty && (millis() - lastSaveReq > 2000)) {
    EEPROM.put(EEPROM_ADDR, cfg);
    dirty = false;
  }
}

// Auto standby: cut the relay after STANDBY_MINUTES of no user activity
void checkStandby() {
  if (STANDBY_MINUTES == 0) return;
  if (!cfg.powerOn) return;
  unsigned long idle = millis() - lastActivity;
  if (idle > (unsigned long)STANDBY_MINUTES * 60000UL) {
    cfg.powerOn = 0;
    applyPower();
    requestSave();
  }
}

// ============================================================================
//  Relay / power
// ============================================================================
void relayWrite(bool on) {
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(PIN_RELAY, level ? HIGH : LOW);
}

void applyPower() {
  relayWrite(cfg.powerOn);
  if (cfg.powerOn) {
    // Re-init the volume IC each time we power the audio stage
    pt2258Init();
    pt2258ApplyAll();
  } else {
    pt2258SetMute(true);   // safety: mute output when powering down
  }
}

void togglePower() {
  cfg.powerOn = !cfg.powerOn;
  applyPower();
  requestSave();
  noteActivity();
}

// ============================================================================
//  Volume actions
// ============================================================================
uint8_t* selectedAtten() {
  return (selection == SEL_MASTER) ? &cfg.masterAtten : &cfg.chAtten[selection];
}

void applySelected() {
  if (selection == SEL_MASTER) pt2258SetMaster(cfg.masterAtten);
  else                         pt2258SetChannel(selection, cfg.chAtten[selection]);
}

void volumeUp() {                 // louder => LESS attenuation
  if (!cfg.powerOn) return;
  uint8_t* a = selectedAtten();
  if (*a > 0) (*a)--;
  applySelected();
  requestSave();
  noteActivity();
}

void volumeDown() {               // quieter => MORE attenuation
  if (!cfg.powerOn) return;
  uint8_t* a = selectedAtten();
  if (*a < 79) (*a)++;
  applySelected();
  requestSave();
  noteActivity();
}

void cycleSelection(bool forward) {
  if (forward) selection = (selection + 1) % SEL_COUNT;
  else         selection = (selection + SEL_COUNT - 1) % SEL_COUNT;
  noteActivity();
}

void toggleMute() {
  if (!cfg.powerOn) return;
  cfg.muted = !cfg.muted;
  pt2258SetMute(cfg.muted);
  requestSave();
  noteActivity();
}

void selectChannel(uint8_t sel) {
  if (sel < SEL_COUNT) selection = sel;
  noteActivity();
}

// Apply a tone preset: overwrite all channel attenuations and push to PT2258
void applyPreset(uint8_t p) {
  if (p >= PRESET_COUNT) return;
  cfg.preset = p;
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    cfg.chAtten[ch] = PRESET_ATTEN[p][ch];
    if (cfg.powerOn) pt2258SetChannel(ch, cfg.chAtten[ch]);
  }
  requestSave();
  noteActivity();
}

void cyclePreset() { applyPreset((cfg.preset + 1) % PRESET_COUNT); }

// ============================================================================
//  Voltage divider
// ============================================================================
void readVoltageDivider() {
  int v = analogRead(PIN_VDIV);
  uint8_t band = 4;
  for (uint8_t i = 0; i < 5; i++) {
    if (v <= VDIV_THRESHOLD[i]) { band = i; break; }
  }
  vdivBand = band;
}

// ============================================================================
//  Rotary encoder (polled quadrature) + push switch
//  Rotate  -> volume up/down on the selected channel
//  Press   -> cycle channel selection (same as Channel-Select button)
// ============================================================================
uint8_t  encPrev = 0;                 // previous 2-bit state (CLK<<1 | DT)
Button   btnEnc{PIN_ENC_SW, HIGH, HIGH, 0};

void serviceEncoder() {
  uint8_t clk = digitalRead(PIN_ENC_CLK);
  uint8_t dt  = digitalRead(PIN_ENC_DT);
  uint8_t state = (clk << 1) | dt;

  // Detect one detent on the CLK falling edge
  if (state != encPrev) {
    // CLK went high->low
    if ((encPrev & 0x02) && !(state & 0x02)) {
      if (dt) volumeUp();     // direction from DT level
      else    volumeDown();
    }
    encPrev = state;
  }

  if (buttonPressed(btnEnc)) cycleSelection(true);
}

// ============================================================================
//  OLED rendering
// ============================================================================
// Convert attenuation (0..79) to a displayed dB value (0..-79)
int attenToDb(uint8_t a) { return -(int)a; }

void drawUI() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ---- Header: power + source label ----
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(cfg.powerOn ? F("ON ") : F("OFF"));
  display.print(F("  SRC:"));
  display.print(pflash(VDIV_LABELS, vdivBand));

  if (cfg.muted) {
    display.setCursor(104, 0);
    display.print(F("MUTE"));
  }
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // ---- Selected channel name (big) ----
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(pflash(CH_NAME, selection));

  // ---- dB value (big) ----
  uint8_t a = (selection == SEL_MASTER) ? cfg.masterAtten : cfg.chAtten[selection];
  display.setTextSize(2);
  display.setCursor(0, 36);
  display.print(attenToDb(a));
  display.print(F(" dB"));

  // ---- Active tone preset (small, top-right of value area) ----
  display.setTextSize(1);
  display.setCursor(92, 16);
  display.print(pflash(PRESET_NAME, cfg.preset));

  // ---- Volume bar (0 dB full .. -79 dB empty) ----
  int barW = map(79 - a, 0, 79, 0, 124);
  display.drawRect(0, 56, 128, 8, SSD1306_WHITE);
  if (barW > 0) display.fillRect(2, 58, barW, 4, SSD1306_WHITE);

  display.display();
}

// ============================================================================
//  IR handling
// ============================================================================
bool irLearnMode = false;

void handleIrCommand(uint16_t cmd) {
  switch (cmd) {
    case IR_POWER:      togglePower();            break;
    case IR_MUTE:       toggleMute();             break;
    case IR_VOL_UP:     volumeUp();               break;
    case IR_VOL_DN:     volumeDown();             break;
    case IR_CH_NEXT:    cycleSelection(true);     break;
    case IR_CH_PREV:    cycleSelection(false);    break;
    case IR_PRESET:     cyclePreset();            break;
    case IR_SEL_FL:     selectChannel(CH_FL);     break;
    case IR_SEL_FR:     selectChannel(CH_FR);     break;
    case IR_SEL_CENTER: selectChannel(CH_CENTER); break;
    case IR_SEL_SUB:    selectChannel(CH_SUB);    break;
    case IR_SEL_RL:     selectChannel(CH_RL);     break;
    case IR_SEL_RR:     selectChannel(CH_RR);     break;
    case IR_SEL_MASTER: selectChannel(SEL_MASTER);break;
    default: break;
  }
}

void serviceIR() {
  if (!IrReceiver.decode()) return;

  uint16_t cmd     = IrReceiver.decodedIRData.command;
  uint16_t address = IrReceiver.decodedIRData.address;
  bool     repeat  = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;

  if (irLearnMode) {
    // Print codes so the user can copy them into the #defines above
    Serial.print(F("[LEARN] addr=0x"));
    Serial.print(address, HEX);
    Serial.print(F("  cmd=0x"));
    Serial.print(cmd, HEX);
    Serial.print(F("  proto="));
    Serial.println(getProtocolString(IrReceiver.decodedIRData.protocol));
  } else {
    // Allow repeat only for volume keys (hold to ramp)
    if (repeat) {
      if (cmd == 0) cmd = 0xFFFF;  // some libs report 0 on repeat
    }
    handleIrCommand(cmd);
  }

  IrReceiver.resume();
}

// ============================================================================
//  Serial command console
// ============================================================================
void printHelp() {
  Serial.println(F("\n=== 5.1 PT2258 Controller ==="));
  Serial.println(F("Serial commands:"));
  Serial.println(F("  L : toggle IR LEARN mode (prints remote codes)"));
  Serial.println(F("  P : toggle power/relay"));
  Serial.println(F("  M : toggle mute"));
  Serial.println(F("  + : volume up (selected)"));
  Serial.println(F("  - : volume down (selected)"));
  Serial.println(F("  > : next channel   < : prev channel"));
  Serial.println(F("  T : cycle tone preset (FLAT/MOVIE/MUSIC/NIGHT)"));
  Serial.println(F("  ? : this help"));
}

void serviceSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 'L': case 'l':
      irLearnMode = !irLearnMode;
      Serial.print(F("IR learn mode: "));
      Serial.println(irLearnMode ? F("ON  (press remote keys)") : F("OFF"));
      break;
    case 'P': case 'p': togglePower();         break;
    case 'M': case 'm': toggleMute();          break;
    case '+':           volumeUp();            break;
    case '-':           volumeDown();          break;
    case '>':           cycleSelection(true);  break;
    case '<':           cycleSelection(false); break;
    case 'T': case 't':
      cyclePreset();
      Serial.print(F("Preset: ")); Serial.println(pflash(PRESET_NAME, cfg.preset));
      break;
    case '?':           printHelp();           break;
    default: break;
  }
}

// ============================================================================
//  Startup splash
// ============================================================================
void drawSplash() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Framed title
  display.drawRoundRect(2, 4, 124, 40, 4, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(30, 10);
  display.print(F("5.1 CH"));
  display.setTextSize(1);
  display.setCursor(14, 30);
  display.print(F("DIGITAL VOLUME"));

  display.setCursor(10, 50);
  display.print(F("PT2258 . Nano"));

  // little VU-style bars
  for (uint8_t i = 0; i < 6; i++) {
    display.fillRect(96 + i * 5, 52 - (i * 2), 3, 4 + i * 2, SSD1306_WHITE);
  }
  display.display();
}

// ============================================================================
//  I2C / OLED helpers
// ============================================================================
// Scan the I2C bus and print found addresses (debug aid on the Serial Monitor)
void i2cScan() {
  Serial.println(F("I2C scan:"));
  uint8_t count = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found device at 0x"));
      Serial.println(a, HEX);
      count++;
    }
  }
  if (count == 0)
    Serial.println(F("  (none found - check SDA/SCL wiring, 3.3-5V power, GND)"));
}

// Start the OLED, trying the two common addresses 0x3C then 0x3D
bool oledBegin() {
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println(F("OLED @0x3C")); return true; }
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) { Serial.println(F("OLED @0x3D")); return true; }
  return false;
}

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_BTN_VOL_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_VOL_DN, INPUT_PULLUP);
  pinMode(PIN_BTN_CH_SEL, INPUT_PULLUP);
  pinMode(PIN_BTN_MUTE,   INPUT_PULLUP);
  pinMode(PIN_BTN_POWER,  INPUT_PULLUP);
  pinMode(PIN_ENC_CLK,    INPUT_PULLUP);
  pinMode(PIN_ENC_DT,     INPUT_PULLUP);
  pinMode(PIN_ENC_SW,     INPUT_PULLUP);
  pinMode(PIN_RELAY,      OUTPUT);
  relayWrite(false);

  encPrev = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);

  Wire.begin();
  Wire.setClock(100000);   // PT2258 is happy at 100kHz

  // OLED - scan the bus (debug) then try 0x3C and 0x3D
  i2cScan();
  oledOk = oledBegin();
  if (!oledOk) {
    Serial.println(F("SSD1306 not found at 0x3C or 0x3D"));
    Serial.println(F("  -> check: SDA=A4, SCL=A5, VCC, GND, and module address"));
  }

  // Startup splash screen (only if the OLED initialised)
  if (oledOk) drawSplash();

  // IR receiver
  IrReceiver.begin(PIN_IR_RECV, ENABLE_LED_FEEDBACK);

  // Config + volume IC
  loadSettings();
  pt2258Init();
  applyPower();          // restores relay + pushes all attenuations
  pt2258ApplyAll();

  printHelp();
  lastActivity = millis();
  delay(1800);           // hold the splash briefly
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
  // ---- Buttons ----
  if (buttonPressed(btnPower)) togglePower();

  if (buttonPressed(btnChSel)) cycleSelection(true);
  if (buttonPressed(btnMute))  toggleMute();
  if (buttonPressed(btnVolUp)) volumeUp();
  if (buttonPressed(btnVolDn)) volumeDown();

  // ---- Rotary encoder ----
  serviceEncoder();

  // ---- IR + Serial ----
  serviceIR();
  serviceSerial();

  // ---- Voltage divider ----
  static unsigned long lastVdiv = 0;
  if (millis() - lastVdiv > 150) { readVoltageDivider(); lastVdiv = millis(); }

  // ---- Display ----
  static unsigned long lastDraw = 0;
  if (oledOk && millis() - lastDraw > 60) { drawUI(); lastDraw = millis(); }

  // ---- Persist + auto standby ----
  maybeSave();
  checkStandby();
}
