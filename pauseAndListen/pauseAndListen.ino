/*
  DFPlayer Mini + Arduino Nano
  Auto-volume control using MAX4466 microphone (analog AC signal centered at Vcc/2)
  with inaudible "micro listen windows" by briefly muting a Class-D BTL amp (e.g., MAX9744).

  Approach:
    - Every ~1.1 s, stop Class-D switching for ~6 ms via /SHDN, sample mic fast, compute peak-to-peak (P2P),
      update smoothed ambient estimate (EMA) with clipping guard and asymmetric smoothing,
      map to a target DFPlayer volume, and step one click toward it (with dwell).

  Hardware:
    - DFPlayer Mini on SoftwareSerial (pins 10=RX, 11=TX).
    - MAX9744 (or similar Class-D): /SHDN (active LOW) wired to Arduino digital pin 7 (configurable below).
      Speakers wired BTL (L+/L-, R+/R-); neither speaker terminal to GND.
    - MAX4466: OUT -> A0 (optionally through small series resistor ~1 kΩ) and small shunt C (220 pF–1 nF) from A0->GND recommended.
      VCC decoupling near mic (100 nF + ~10 µF).
    - Buttons on pins 3, 4, 5 with INPUT_PULLUP (active LOW): [prevVol, play, nextVol].
*/

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <math.h>

// ---------------- Buttons / DFPlayer ----------------
void buttonHandler();

const uint8_t buttonPins[] = {3, 4, 5}; // [prevVol, play, nextVol]
const int debounceTime = 50;
const int numTracks = 4;
const int dfpMaxVolume = 30;

int buttons[] = {0, 0, 0};
int prevButtons[] = {0, 0, 0};
int prevButtonTimes[] = {0, 0, 0};

int currentTrack = 1;
int currentVolume = 20;

SoftwareSerial mySerial(10, 11); // RX, TX (Arduino pin10<-DFPlayer TX, pin11->DFPlayer RX)
DFRobotDFPlayerMini myDFPlayer;

// ---------------- Microphone / Auto-volume ----------------
const int MIC_PIN = A0;              // MAX4466 OUT
float ambientEMA = 0.0f;             // smoothed ambient P2P

// Asymmetric EMA and guards
const int   CLIP_P2P_THRESHOLD = 1000; // reject saturated windows
const float AMBIENT_DEADBAND   = 3.0f; // ignore tiny changes
const float ALPHA_UP           = 0.20f; // ambient rises: slower
const float ALPHA_DOWN         = 0.60f; // ambient falls: faster

// Volume limits and pacing
const int MIN_VOL = 6;                    // floor in auto mode
const int MAX_VOL = 24;                   // cap below max to reduce distortion
const unsigned long VOL_STEP_DWELL_MS = 300; // min time between 1-step volume changes

// Mapping range (tune after observing serial with amp muted during window)
const int P2P_MIN_EXPECTED = 15;   // quiet room P2P
const int P2P_MAX_EXPECTED = 800;  // noisy room P2P

// Timing
unsigned long lastListenMs = 0;
unsigned long lastVolStepMs = 0;

// Micro-window (inaudible)
const unsigned long LISTEN_WINDOW_MS = 6;       // ~5–6 ms window
const unsigned long LISTEN_PERIOD_MS = 1100;    // period between windows
const uint16_t      LISTEN_JITTER_MS = 250;     // ± jitter so ear can’t lock to a rhythm
unsigned long nextListenDue = 0;

// Amp SHDN (MAX9744 /SHDN active LOW)
const int AMP_SHDN_PIN = 7;

// ---------------- Utilities ----------------
int mapClamped(int x, int in_min, int in_max, int out_min, int out_max) {
  if (x < in_min) x = in_min;
  if (x > in_max) x = in_max;
  long num = (long)(x - in_min) * (out_max - out_min);
  long den = (in_max - in_min);
  int y = out_min + (den == 0 ? 0 : (int)(num / den));
  if (y < out_min) y = out_min;
  if (y > out_max) y = out_max;
  return y;
}

// Fast P2P sampler using micros() for tight window
int fastReadMicP2P(unsigned long window_ms) {
  int minV = 1023, maxV = 0;
  unsigned long endT = micros() + window_ms * 1000UL;
  while ((long)(endT - micros()) > 0) {
    int v = analogRead(MIC_PIN);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  return maxV - minV;
}

void setup() {
  Serial.begin(9600);
  mySerial.begin(9600);

  // Buttons
  for (int i = 0; i < 3; i++) pinMode(buttonPins[i], INPUT_PULLUP);

  // Amp SHDN control
  pinMode(AMP_SHDN_PIN, OUTPUT);
  digitalWrite(AMP_SHDN_PIN, HIGH); // amp ON by default

  // DFPlayer init
  if (!myDFPlayer.begin(mySerial)) {
    Serial.println("DFPlayer Mini not detected!");
    while (true);
  }
  Serial.println("DFPlayer Mini ready!");
  myDFPlayer.volume(currentVolume);  // 0..30
  Serial.println("Playing File 1");
  myDFPlayer.play(currentTrack);

  // Initialize ambient baseline with a brief clean read (full window)
  delay(400);
  digitalWrite(AMP_SHDN_PIN, LOW);               // stop Class-D switching
  delayMicroseconds(800);                        // small settle
  int baseP2P = fastReadMicP2P(10);             // ~10 ms baseline
  digitalWrite(AMP_SHDN_PIN, HIGH);             // resume amp
  ambientEMA = baseP2P;

  lastListenMs   = millis();
  lastVolStepMs  = millis();
  randomSeed(analogRead(A3));                   // jitter seed
  nextListenDue  = millis() + 800;

  Serial.println("Auto-volume initialized");
}

void loop() {
  // Buttons: -1=just released, 0=not pressed, 1=held, 2=just pressed
  buttonHandler();

  // Manual volume control
  if (buttons[2] > 0) { // nextVol (increase)
    if (currentVolume + 1 <= dfpMaxVolume) {
      currentVolume++;
      myDFPlayer.volume(currentVolume);
      lastVolStepMs = millis();
      Serial.print("Manual vol up -> ");
      Serial.println(currentVolume);
    }
  } else if (buttons[0] > 0) { // prevVol (decrease)
    if (currentVolume - 1 >= 0) {
      currentVolume--;
      myDFPlayer.volume(currentVolume);
      lastVolStepMs = millis();
      Serial.print("Manual vol down -> ");
      Serial.println(currentVolume);
    }
  }

  // Middle button: simple play/pause "toggle"
  if (buttons[1] == 2) {
    myDFPlayer.pause();
    delay(120);
    myDFPlayer.start();
    Serial.println("Play/Pause toggle");
  }

  // -------- Auto-volume: micro listen window (no audible pause) --------
  unsigned long now = millis();
  if ((long)(now - nextListenDue) >= 0) {
    // Stop Class-D switching briefly
    digitalWrite(AMP_SHDN_PIN, LOW);         // amp OFF
    delayMicroseconds(500);                  // settle edges
    int p2p = fastReadMicP2P(LISTEN_WINDOW_MS);
    digitalWrite(AMP_SHDN_PIN, HIGH);        // amp ON

    // Reject clipped windows; asymmetric EMA (fast down, slow up)
    bool clipped = (p2p >= CLIP_P2P_THRESHOLD);
    int sampleP2P = clipped ? (int)ambientEMA : p2p;
    float d = (float)sampleP2P - ambientEMA;
    if (fabsf(d) > AMBIENT_DEADBAND) {
      float a = (d > 0) ? ALPHA_UP : ALPHA_DOWN;
      ambientEMA = (1.0f - a) * ambientEMA + a * (float)sampleP2P;
    }
    lastListenMs = now;

    // Map EMA to target volume
    int targetVol = mapClamped((int)ambientEMA,
                               P2P_MIN_EXPECTED, P2P_MAX_EXPECTED,
                               MIN_VOL, MAX_VOL);
    targetVol = constrain(targetVol, 0, dfpMaxVolume);

    // Step slowly toward target to avoid pumping
    if (now - lastVolStepMs >= VOL_STEP_DWELL_MS) {
      if (targetVol > currentVolume) {
        currentVolume++;
        myDFPlayer.volume(currentVolume);
        lastVolStepMs = now;
      } else if (targetVol < currentVolume) {
        currentVolume--;
        myDFPlayer.volume(currentVolume);
        lastVolStepMs = now;
      }
    }

    // Debug
    Serial.print("P2P=");       Serial.print(p2p);
    Serial.print(" EMA=");      Serial.print(ambientEMA, 1);
    Serial.print(" targetVol=");Serial.print(targetVol);
    Serial.print(" currVol=");  Serial.println(currentVolume);

    // Jitter next schedule so ear can't lock to a rhythm
    long j = (long)random(-(long)LISTEN_JITTER_MS, (long)LISTEN_JITTER_MS);
    nextListenDue = now + LISTEN_PERIOD_MS + j;
  }

  delay(50);
}

// ---------------- Button handler (debounced state machine) ----------------
void buttonHandler() { // -1=just released, 0=not pressed, 1=held down, 2=just pressed
  for (int i = 0; i < 3; i++) {
    int b = !digitalRead(buttonPins[i]); // active LOW
    if (b == HIGH) { // held
      if (prevButtons[i] <= 0) { // not previously held
        if ((millis() - prevButtonTimes[i] >= debounceTime)) {
          buttons[i] = 2; // just pressed
          prevButtonTimes[i] = millis();
        } else {
          buttons[i] = 0; // bounce ignored
        }
      } else {
        buttons[i] = 1; // held
      }
    } else {
      if (prevButtons[i] > 0) {
        buttons[i] = -1; // just released
      } else {
        buttons[i] = 0;  // not pressed
      }
    }
  }

  for (int i = 0; i < 3; i++) {
    prevButtons[i] = (buttons[i] > 0) ? 1 : 0;
  }
}
