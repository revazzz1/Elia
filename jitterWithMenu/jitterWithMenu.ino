/*
  DFPlayer + Auto-Volume (MAX4466) + MAX9744 /SHDN Listen-Window + RGB 1602 LCD Menu
  - Stable selection using /MP3 + playMp3Folder(n)
  - Tracks: 0001..0005 => [Rain, Fireplace, Ocean, Brown, Pink]
  - Alarm:  0099.mp3    (plays once)
  - Uses amp /SHDN for ~6 ms “listen window” every ~1.8 s (with jitter)
  - Dashboard + 2 min auto-return
*/

#include <Arduino.h>
#include <Wire.h>
#include "DFRobot_RGBLCD1602.h"
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <math.h>

// ---------------- Pins ----------------
const uint8_t BTN_LEFT   = 3;
const uint8_t BTN_SELECT = 4;
const uint8_t BTN_RIGHT  = 5;

const int AMP_SHDN_PIN   = 7;   // MAX9744 /SHDN (active LOW)
const int MIC_PIN        = A0;  // MAX4466 OUT

// ---------------- DFPlayer ----------------
SoftwareSerial mySerial(10, 11);           // RX, TX (Arduino pin10<-DF TX, pin11->DF RX)
DFRobotDFPlayerMini myDFPlayer;

// ---------------- LCD ----------------
DFRobot_RGBLCD1602 lcd(16, 2);
static inline void applyBacklight() { lcd.setRGB(255, 255, 255); }

// ---------------- Buttons ----------------
struct Btn {
  uint8_t pin; bool last; bool fell;
  unsigned long lastChangeMs; uint16_t debounceMs;
};
Btn btnL{BTN_LEFT, 1, 0, 0, 35};
Btn btnS{BTN_SELECT,1, 0, 0, 35};
Btn btnR{BTN_RIGHT, 1, 0, 0, 35};

static inline void pollBtnRaw(uint8_t pin, bool &last, bool &fell,
                              unsigned long &lastChangeMs, uint16_t debounceMs) {
  bool r = digitalRead(pin);
  unsigned long now = millis();
  if (r != last && (now - lastChangeMs) >= debounceMs) {
    fell = (last == 1 && r == 0);  // active LOW
    last = r; lastChangeMs = now;
  } else fell = false;
}

// ---------------- Settings / State ----------------
int  set_sens       = 50;     // 0..100; caps allowed max volume
int  set_maxVol     = 20;     // 0..30 DFPlayer cap
int  set_playDurMin = 60;     // 1..480 minutes

// Clock & Alarm
int  alarmH = 7,  alarmM = 30;
int  clockH = 12, clockM = 0;
unsigned long lastMinuteTick = 0;
bool alarmArmed = true;
bool alarmActive = false;

// Playback duration timer
unsigned long playEndAt = 0;   // millis deadline; 0 = not set
bool isPlaying = false;

// ---------------- Auto-Volume (Mic) ----------------
float ambientEMA = 0.0f;
const int   CLIP_P2P_THRESHOLD = 1000;
const float AMBIENT_DEADBAND   = 3.0f;
const float ALPHA_UP           = 0.20f;   // rise slower
const float ALPHA_DOWN         = 0.60f;   // fall faster

// Mapping window for P2P measured with amp muted
const int BASE_P2P_MIN = 20;
const int BASE_P2P_MAX = 120;

const int dfpMaxVolume = 30;
int currentVolume = 20;
const unsigned long VOL_STEP_DWELL_MS = 400;  // gentle
unsigned long lastVolStepMs = 0;

// Listen window timings + jitter (old method)
const unsigned long LISTEN_WINDOW_MS = 6;
const unsigned long LISTEN_PERIOD_MS = 1800;
const uint16_t      LISTEN_JITTER_MS = 300;
unsigned long nextListenDue = 0;

// Target volume (debug)
int gTargetVol = 0;

// ---------------- Tracks / Files ----------------
const int   kFirstTrack = 1;      // 0001.mp3
const int   kLastTrack  = 5;      // 0005.mp3
const int   kTrackCount = (kLastTrack - kFirstTrack + 1);
const int   kAlarmTrack = 99;     // 0099.mp3

int  fileIndex      = 0;          // 0..kTrackCount-1
int  currentTrack   = kFirstTrack;
const char* files[kTrackCount] = { "Rain", "Fireplace", "Ocean", "Brown", "Pink" };

// ---------------- Menu System ----------------
enum MenuItem : uint8_t { MI_DASH=0, MI_SENS, MI_MAXVOL, MI_FILE, MI_PLAYDUR, MI_ALARM, MI_SETTIME, MI_COUNT };
uint8_t menuIndex = MI_DASH;
enum EditMode : uint8_t { EM_NONE=0, EM_EDIT, EM_TIME_HOURS, EM_TIME_MINUTES };
EditMode editMode = EM_NONE;

// LCD blinking
unsigned long blinkMs = 0; bool blinkOn = false;
const unsigned long MENU_REFRESH_MS = 250;
unsigned long lastMenuRefresh = 0;

// Auto return to Dashboard on inactivity (2 minutes)
const unsigned long IDLE_RETURN_MS = 120000UL;
unsigned long lastUserActionMs = 0;

// ---------------- LCD Custom Symbols ----------------
uint8_t iconMic[8]    = {B00100,B00100,B01110,B10101,B10100,B10100,B10101,B01110};
uint8_t iconVol[8]    = {B00001,B00011,B11111,B11111,B11111,B00011,B00001,B00000};
uint8_t iconFile[8]   = {B11111,B10001,B10101,B10101,B10101,B10101,B10001,B11111};
uint8_t iconTimer[8]  = {B00100,B01110,B10101,B10101,B10101,B10101,B01110,B00100};
uint8_t iconAlarmI[8] = {B00100,B10101,B10101,B11111,B01110,B00100,B01010,B10001};
uint8_t iconClock[8]  = {B01110,B10001,B10101,B10101,B10001,B10001,B01110,B00000};
uint8_t iconLArr[8]   = {B00010,B00110,B01110,B11110,B01110,B00110,B00010,B00000};
uint8_t iconRArr[8]   = {B01000,B01100,B01110,B01111,B01110,B01100,B01000,B00000};

// ---------------- Utilities ----------------
static inline int mapClamped(int x, int in_min, int in_max, int out_min, int out_max) {
  if (x < in_min) x = in_min;
  if (x > in_max) x = in_max;
  long num = (long)(x - in_min) * (out_max - out_min);
  long den = (in_max - in_min);
  int y = out_min + (den == 0 ? 0 : (int)(num / den));
  if (y < out_min) y = out_min;
  if (y > out_max) y = out_max;
  return y;
}

static inline int fastReadMicP2P(unsigned long window_ms) {
  int minV = 1023, maxV = 0;
  unsigned long endT = micros() + window_ms * 1000UL;
  while ((long)(endT - micros()) > 0) {
    int v = analogRead(MIC_PIN);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  return maxV - minV;
}

// ---------------- DFPlayer helpers ----------------
void playCurrentTrack() {
  currentTrack = kFirstTrack + fileIndex;   // 1..5
  alarmActive = false;
  Serial.print(F("[PLAY] MP3/ ")); Serial.println(currentTrack);
  myDFPlayer.playMp3Folder(currentTrack);   // deterministic with /MP3
}

void trackAdvance(int delta) {
  fileIndex = (fileIndex + delta + kTrackCount) % kTrackCount;
  playCurrentTrack();
  isPlaying = true;
  playEndAt = millis() + (unsigned long)set_playDurMin * 60000UL;
}

void playAlarmOnce() {
  alarmActive = true;
  Serial.println(F("[ALARM] MP3/ 0099"));
  myDFPlayer.playMp3Folder(kAlarmTrack);
  isPlaying = true;
}

// ---------------- Clock / Alarm ----------------
void serviceClock() {
  unsigned long now = millis();
  if (now - lastMinuteTick >= 60000UL) {
    lastMinuteTick += 60000UL;
    clockM++;
    if (clockM >= 60) { clockM = 0; clockH = (clockH + 1) % 24; }
    Serial.print(F("[CLOCK] "));
    if (clockH<10) Serial.print('0'); Serial.print(clockH);
    Serial.print(':');
    if (clockM<10) Serial.print('0'); Serial.println(clockM);
  }
}
void checkAlarm() {
  static int lastHM = -1;
  int hm = clockH*60 + clockM;
  if (hm != lastHM) {
    lastHM = hm;
    if (alarmArmed && clockH == alarmH && clockM == alarmM) {
      playAlarmOnce();
      alarmArmed = false;
    }
  }
}

// ---------------- LCD Rendering ----------------
void drawTitle(uint8_t iconSlot, const char* title) {
  lcd.setCursor(0,0); lcd.write(iconSlot); lcd.print(" ");
  char buf[14]; memset(buf, 0, sizeof(buf)); strncpy(buf, title, 13); lcd.print(buf);
  lcd.setCursor(15,0); lcd.print((editMode == EM_NONE) ? " " : "*");
}
void drawBar(int value, int vmin, int vmax, int row) {
  value = constrain(value, vmin, vmax);
  lcd.setCursor(0,row);
  lcd.print('[');
  int minDisp = vmin; if (minDisp > 99) minDisp = 99; if (minDisp < 0) minDisp = 0;
  if (minDisp < 10) { lcd.print(' '); lcd.print(minDisp); }
  else              { lcd.print(minDisp/10); lcd.print(minDisp%10); }
  lcd.print(']');
  lcd.print(' ');
  lcd.print('|');
  const int barCols = 8;
  int fill = map(value, vmin, vmax, 0, barCols);
  for (int i=0; i<barCols; ++i) { if (i < fill) lcd.write(byte(255)); else lcd.print(' '); }
  lcd.print('|');
  int valDisp = value; if (valDisp > 999) valDisp = 999; if (valDisp < 0) valDisp = 0;
  char vbuf[4]; snprintf(vbuf, sizeof(vbuf), "%3d", valDisp);
  lcd.setCursor(13,row); lcd.print(vbuf);
}
void drawFilePicker() {
  drawTitle(2, "Sound file");
  lcd.setCursor(0,1); lcd.write((uint8_t)6); lcd.print(" ");
  char buf[10]; memset(buf, 0, sizeof(buf)); strncpy(buf, files[fileIndex], 10);
  lcd.print(buf); lcd.print(" "); lcd.write((uint8_t)7);
}
void drawTimeEdit(const char* title, bool isAlarm) {
  drawTitle(isAlarm ? 4 : 5, title);
  int hh = isAlarm ? alarmH : clockH;
  int mm = isAlarm ? alarmM : clockM;
  bool blinkHH = (editMode == EM_TIME_HOURS)  && blinkOn;
  bool blinkMM = (editMode == EM_TIME_MINUTES)&& blinkOn;

  lcd.setCursor(0,1);  lcd.write((uint8_t)6);
  lcd.setCursor(15,1); lcd.write((uint8_t)7);

  lcd.setCursor(4,1);
  if (blinkHH) lcd.print("  "); else { lcd.print(hh/10); lcd.print(hh%10); }
  lcd.print(":");
  if (blinkMM) lcd.print("  "); else { lcd.print(mm/10); lcd.print(mm%10); }
}
void drawDashboard() {
  // Line 1: icon + file name + time
  lcd.setCursor(0,0); lcd.write((uint8_t)2); lcd.print(" ");
  char nameBuf[10]; memset(nameBuf, 0, sizeof(nameBuf)); strncpy(nameBuf, files[fileIndex], 9);
  lcd.print(nameBuf);
  lcd.setCursor(11,0);
  if (clockH<10) lcd.print('0'); lcd.print(clockH);
  lcd.print(':');
  if (clockM<10) lcd.print('0'); lcd.print(clockM);

  // Line 2: Vol cur/limit, target, tiny bar for ambientEMA
  lcd.setCursor(0,1); lcd.write((uint8_t)1); lcd.print(" V");
  if (currentVolume<10) lcd.print('0'); lcd.print(currentVolume);
  lcd.print('/');
  if (set_maxVol<10) lcd.print('0'); lcd.print(set_maxVol);
  lcd.print(' ');
  lcd.print('T');
  if (gTargetVol<10) lcd.print('0'); lcd.print(gTargetVol);

  int barCols = 5;
  int fill = map((int)ambientEMA, BASE_P2P_MIN, BASE_P2P_MAX, 0, barCols);
  if (fill < 0) fill = 0; if (fill > barCols) fill = barCols;
  lcd.print(' ');
  for (int i=0;i<barCols;i++) { if (i < fill) lcd.write(byte(255)); else lcd.print(' '); }
}
void renderMenu() {
  lcd.clear();
  switch (menuIndex) {
    case MI_DASH:    drawDashboard(); break;
    case MI_SENS:    drawTitle(0, "Sensitivity");  drawBar(set_sens, 0, 100, 1); break;
    case MI_MAXVOL:  drawTitle(1, "Max volume");   drawBar(set_maxVol, 0, 30, 1); break;
    case MI_FILE:    drawFilePicker(); break;
    case MI_PLAYDUR: drawTitle(3, "Play minutes"); drawBar(set_playDurMin, 1, 480, 1); break;
    case MI_ALARM:   drawTimeEdit("Alarm", true);  break;
    case MI_SETTIME: drawTimeEdit("Set time", false); break;
  }
}

// ---------------- Menu Logic ----------------
void applyAdjustmentPrimary(int dir) {
  switch (menuIndex) {
    case MI_SENS:
      set_sens = constrain(set_sens + dir*2, 0, 100);
      Serial.print(F("[MENU] Sens=")); Serial.println(set_sens);
      break;
    case MI_MAXVOL:
      set_maxVol = constrain(set_maxVol + dir, 0, 30);
      Serial.print(F("[MENU] MaxVol=")); Serial.println(set_maxVol);
      if (currentVolume > set_maxVol) { currentVolume = set_maxVol; myDFPlayer.volume(currentVolume);
        Serial.print(F("[VOLUME] Cap applied -> ")); Serial.println(currentVolume); }
      break;
    case MI_FILE: {
      fileIndex = (fileIndex + dir + kTrackCount) % kTrackCount;
      Serial.print(F("[MENU] File=")); Serial.print(files[fileIndex]);
      Serial.print(F(" -> track "));   Serial.println(kFirstTrack + fileIndex);
      playCurrentTrack();
      isPlaying = true;
      playEndAt = millis() + (unsigned long)set_playDurMin * 60000UL;
      break;
    }
    case MI_PLAYDUR:
      set_playDurMin = constrain(set_playDurMin + dir*5, 1, 480);
      Serial.print(F("[MENU] PlayDur(min)=")); Serial.println(set_playDurMin);
      if (isPlaying && playEndAt) playEndAt = millis() + (unsigned long)set_playDurMin * 60000UL;
      break;
    case MI_ALARM:
      if (editMode == EM_EDIT || editMode == EM_TIME_HOURS) { alarmH = (alarmH + dir + 24) % 24; editMode = EM_TIME_HOURS;
        Serial.print(F("[MENU] AlarmH=")); Serial.println(alarmH); }
      break;
    case MI_SETTIME:
      if (editMode == EM_EDIT || editMode == EM_TIME_HOURS) { clockH = (clockH + dir + 24) % 24; editMode = EM_TIME_HOURS;
        Serial.print(F("[MENU] ClockH=")); Serial.println(clockH); }
      break;
    case MI_DASH: default: break;
  }
}
void applyAdjustmentSecondary(int dir) {
  switch (menuIndex) {
    case MI_ALARM:   alarmM = (alarmM + dir + 60) % 60; lastMinuteTick = millis();
                     Serial.print(F("[MENU] AlarmM=")); Serial.println(alarmM); break;
    case MI_SETTIME: clockM = (clockM + dir + 60) % 60; lastMinuteTick = millis();
                     Serial.print(F("[MENU] ClockM=")); Serial.println(clockM); break;
    default: applyAdjustmentPrimary(dir); break;
  }
}
void handleMenuInput() {
  if (millis() - blinkMs >= 400) { blinkMs = millis(); blinkOn = !blinkOn; }

  pollBtnRaw(btnL.pin, btnL.last, btnL.fell, btnL.lastChangeMs, btnL.debounceMs);
  pollBtnRaw(btnS.pin, btnS.last, btnS.fell, btnS.lastChangeMs, btnS.debounceMs);
  pollBtnRaw(btnR.pin, btnR.last, btnR.fell, btnR.lastChangeMs, btnR.debounceMs);

  bool anyPress = (btnL.fell || btnS.fell || btnR.fell);
  if (anyPress) lastUserActionMs = millis();

  bool changed = false;

  if (editMode == EM_NONE) {
    if (btnL.fell) { menuIndex = (menuIndex + MI_COUNT - 1) % MI_COUNT; changed = true; Serial.print(F("[MENU] Index=")); Serial.println(menuIndex); }
    if (btnR.fell) { menuIndex = (menuIndex + 1) % MI_COUNT;         changed = true; Serial.print(F("[MENU] Index=")); Serial.println(menuIndex); }
    if (btnS.fell) { 
      if (menuIndex==MI_ALARM||menuIndex==MI_SETTIME) editMode = EM_TIME_HOURS;
      else if (menuIndex!=MI_DASH) editMode = EM_EDIT;
      changed = true; Serial.println(F("[MENU] Edit ON")); 
    }
  } else {
    if (menuIndex == MI_ALARM || menuIndex == MI_SETTIME) {
      if (btnL.fell) { if (editMode==EM_TIME_HOURS) applyAdjustmentPrimary(-1); else applyAdjustmentSecondary(-5); changed = true; }
      if (btnR.fell) { if (editMode==EM_TIME_HOURS) applyAdjustmentPrimary(+1); else applyAdjustmentSecondary(+5); changed = true; }
      if (btnS.fell) { if (editMode==EM_TIME_HOURS) { editMode=EM_TIME_MINUTES; Serial.println(F("[MENU] Edit Minutes")); }
                       else { editMode=EM_NONE; changed = true; Serial.println(F("[MENU] Edit OFF")); } }
    } else {
      if (btnL.fell) { applyAdjustmentPrimary(-1); changed = true; }
      if (btnR.fell) { applyAdjustmentPrimary(+1); changed = true; }
      if (btnS.fell) { editMode = EM_NONE; changed = true; Serial.println(F("[MENU] Edit OFF")); }
    }
  }

  if (changed) { renderMenu(); lastMenuRefresh = millis(); }

  // During time edit, refresh periodically to update blinking cleanly.
  if ((menuIndex == MI_ALARM || menuIndex == MI_SETTIME) &&
      (editMode == EM_TIME_HOURS || editMode == EM_TIME_MINUTES)) {
    if (millis() - lastMenuRefresh >= MENU_REFRESH_MS) {
      renderMenu(); lastMenuRefresh = millis();
    }
  }

  // Idle auto-return to Dashboard (except if already on Dashboard or in edit)
  if (editMode == EM_NONE && menuIndex != MI_DASH) {
    if (millis() - lastUserActionMs >= IDLE_RETURN_MS) {
      menuIndex = MI_DASH; renderMenu();
      Serial.println(F("[MENU] Auto-return to Dashboard"));
    }
  }
}

// ---------------- Auto-Volume (old /SHDN listen window) ----------------
void autoVolumeTick() {
  unsigned long now = millis();
  if ((long)(now - nextListenDue) >= 0) {

    // Briefly shut down amp so mic only hears the room
    digitalWrite(AMP_SHDN_PIN, LOW);          // amp OFF
    delayMicroseconds(500);                   // settle
    int p2p = fastReadMicP2P(LISTEN_WINDOW_MS);
    digitalWrite(AMP_SHDN_PIN, HIGH);         // amp ON

    // EMA with deadband
    bool clipped = (p2p >= CLIP_P2P_THRESHOLD);
    int sampleP2P = clipped ? (int)ambientEMA : p2p;
    float d = (float)sampleP2P - ambientEMA;
    if (fabs(d) > AMBIENT_DEADBAND) {
      float a = (d > 0) ? ALPHA_UP : ALPHA_DOWN;
      ambientEMA = (1.0f - a) * ambientEMA + a * (float)sampleP2P;
    }

    // Map to volume
    const int p2pMin = BASE_P2P_MIN;
    const int p2pMax = BASE_P2P_MAX;
    int targetVol = mapClamped((int)ambientEMA, p2pMin, p2pMax, 6, 24);

    // Sensitivity caps allowed max; 0..100 -> 8..30
    int sensCap = mapClamped(set_sens, 0, 100, 8, 30);
    int effectiveCap = min(set_maxVol, sensCap);

    targetVol = constrain(targetVol, 0, effectiveCap);
    targetVol = constrain(targetVol, 0, dfpMaxVolume);
    gTargetVol = targetVol;

    // Debug
    Serial.print(F("[DBG] cur="));   Serial.print(currentVolume);
    Serial.print(F(" tgt="));        Serial.print(gTargetVol);
    Serial.print(F(" p2p="));        Serial.print(p2p);
    Serial.print(F(" ema="));        Serial.print(ambientEMA,1);
    Serial.print(F(" map=["));       Serial.print(p2pMin); Serial.print(".."); Serial.print(p2pMax);
    Serial.print(F("] cap="));       Serial.print(effectiveCap);
    Serial.println();

    // Step toward target
    if (now - lastVolStepMs >= VOL_STEP_DWELL_MS) {
      if (targetVol > currentVolume) {
        currentVolume++; myDFPlayer.volume(currentVolume); lastVolStepMs = now;
      } else if (targetVol < currentVolume) {
        currentVolume--; myDFPlayer.volume(currentVolume); lastVolStepMs = now;
      }
    }

    // Schedule next window with jitter
    long j = (long)random(-(long)LISTEN_JITTER_MS, (long)LISTEN_JITTER_MS);
    nextListenDue = now + LISTEN_PERIOD_MS + j;

    // Keep dashboard fresh
    if (menuIndex == MI_DASH && (now - lastMenuRefresh >= 500)) {
      renderMenu(); lastMenuRefresh = now;
    }
  }
}

// ---------------- Playback wrappers ----------------
void startPlayback() {
  playCurrentTrack();
  isPlaying = true;
  playEndAt = millis() + (unsigned long)set_playDurMin * 60000UL;
}
void pausePlayback() {
  Serial.println(F("[PAUSE]"));
  myDFPlayer.pause();
  isPlaying = false;
  playEndAt = 0;
}

// ---------------- Debug table ----------------
void debugPrintTrackTable() {
  int folders = myDFPlayer.readFolderCounts();
  Serial.print(F("[SD] DFPlayer folders: ")); Serial.println(folders);
  Serial.println(F("[SD] /MP3 declared table:"));
  for (int i = 0; i < kTrackCount; ++i) {
    int tn = kFirstTrack + i; char num[6]; snprintf(num, sizeof(num), "%04d", tn);
    Serial.print(F("  #")); Serial.print(tn);
    Serial.print(F("  MP3/")); Serial.print(num); Serial.print(F(".mp3 -> "));
    Serial.println(files[i]);
  }
  char alarmNum[6]; snprintf(alarmNum, sizeof(alarmNum), "%04d", kAlarmTrack);
  Serial.print(F("  Alarm MP3/")); Serial.print(alarmNum); Serial.println(F(".mp3 -> Alarm sound"));
}

// ---------------- Setup / Loop ----------------
void setup() {
  pinMode(BTN_LEFT,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_RIGHT,  INPUT_PULLUP);

  pinMode(AMP_SHDN_PIN, OUTPUT);
  digitalWrite(AMP_SHDN_PIN, HIGH); // amp ON

  Serial.begin(9600);
  Serial.println(F("\n=== BOOT ==="));

  mySerial.begin(9600);

  // LCD init
  lcd.init(); applyBacklight(); lcd.clear();
  lcd.customSymbol(0, iconMic);
  lcd.customSymbol(1, iconVol);
  lcd.customSymbol(2, iconFile);
  lcd.customSymbol(3, iconTimer);
  lcd.customSymbol(4, iconAlarmI);
  lcd.customSymbol(5, iconClock);
  lcd.customSymbol(6, iconLArr);
  lcd.customSymbol(7, iconRArr);

  // DFPlayer init
  Serial.println(F("[DFP] init..."));
  if (!myDFPlayer.begin(mySerial)) {
    Serial.println(F("[DFP] init FAIL"));
    lcd.setCursor(0,0); lcd.print("DFP init fail");
    while (true);
  }
  myDFPlayer.setTimeOut(500);
  myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  delay(200);
  myDFPlayer.volume(currentVolume);
  delay(200);

  debugPrintTrackTable();

  // Start first track (0001) from /MP3
  fileIndex = 0; startPlayback();

  // Baseline ambient (with amp muted once)
  delay(400);
  digitalWrite(AMP_SHDN_PIN, LOW);
  delayMicroseconds(800);
  int baseP2P = fastReadMicP2P(10);
  digitalWrite(AMP_SHDN_PIN, HIGH);
  ambientEMA = baseP2P;
  Serial.print(F("[MIC] baseline P2P=")); Serial.println(baseP2P);

  randomSeed(analogRead(A3));
  unsigned long now = millis();
  nextListenDue   = now + 800;
  lastVolStepMs   = now;
  lastMinuteTick  = now;
  lastMenuRefresh = now;
  lastUserActionMs= now;

  renderMenu();
  lcd.setCursor(0,0); lcd.print("Sound: "); lcd.print(files[fileIndex]);
  lcd.setCursor(0,1); lcd.print("Vol "); lcd.print(currentVolume); lcd.print("/"); lcd.print(set_maxVol);
  Serial.println(F("[BOOT] ready"));
}

void loop() {
  serviceClock();
  checkAlarm();

  // DFPlayer events (auto-next and alarm handling)
  if (myDFPlayer.available()) {
    uint8_t type = myDFPlayer.readType();
    uint16_t val = myDFPlayer.read();
    if (type == DFPlayerPlayFinished) {
      Serial.print(F("[DFP] finished ")); Serial.println(val);
      if (alarmActive) { alarmActive = false; pausePlayback(); Serial.println(F("[ALARM] done -> paused")); }
      else { trackAdvance(+1); }
    }
  }

  if (isPlaying && playEndAt && (long)(millis() - playEndAt) >= 0 && !alarmActive) {
    Serial.println(F("[PLAY] duration ended -> pause")); pausePlayback();
  }

  handleMenuInput();
  autoVolumeTick();

  delay(10);
}
