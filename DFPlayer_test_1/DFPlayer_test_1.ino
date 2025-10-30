/*
  DFPlayer Mini + Arduino Nano
  Auto-volume control using MAX4466 microphone (analog AC signal centered at Vcc/2).
  Approach:
    - Brief "listen windows": pause DFPlayer briefly, measure room noise via peak-to-peak amplitude,
      resume playback immediately to avoid measuring self-audio.
    - Exponential smoothing, hysteresis, and slow step changes to prevent audible pumping.
    - Manual buttons remain active for volume up/down and play/pause/track controls.

  Hardware:
    - DFPlayer Mini connected via SoftwareSerial (pins 10=RX, 11=TX).
    - MAX4466 OUT -> A0, VCC -> 5V (or 3.3V), GND -> GND.
    - Buttons on pins 3, 4, 5 with INPUT_PULLUP (active LOW): [prevVol, play, nextVol].

  Notes:
    - The MAX4466 outputs an AC waveform around mid-rail (~512 on 10-bit ADC). Loudness is measured
      as peak-to-peak amplitude during the listen window.
    - Adjust the MAX4466 gain trimpot so quiet room yields small P2P and loud sounds do not clip.
    - Tune map ranges and limits to match the environment and preferred behavior.
*/

#include "SoftwareSerial.h"
#include "DFRobotDFPlayerMini.h"

// ---------- Buttons / DFPlayer config ----------
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

SoftwareSerial mySerial(10, 11); // RX, TX (Arduino pin10->DFPlayer TX, pin11->DFPlayer RX)
DFRobotDFPlayerMini myDFPlayer;

// ---------- Microphone / auto-volume config ----------
const int MIC_PIN = A0;

// How often to sample ambient (ms)
const unsigned long LISTEN_PERIOD_MS = 1200;

// How long to pause playback and measure (ms). Keep short to be inaudible in steady rain.
const unsigned long LISTEN_WINDOW_MS = 25;

// Smoothing and control parameters
float ambientEMA = 0.0f;
const float AMBIENT_ALPHA = 0.20f;      // EMA smoothing factor (0..1)
const float AMBIENT_DEADBAND = 5.0f;    // Ignore tiny changes in P2P

// Volume limits and pacing
const int MIN_VOL = 8;                  // Never drop below this in auto mode
const int MAX_VOL = 28;                 // Cap below DFPlayer max to reduce distortion
const unsigned long VOL_STEP_DWELL_MS = 350; // Minimum interval between volume step changes

// Mapping ranges for P2P amplitude from MAX4466 (tune to environment)
// Example assumption: quiet room ~ 0..80 P2P, noisy room ~ 300..600 P2P
const int P2P_MIN_EXPECTED = 0;
const int P2P_MAX_EXPECTED = 600;

unsigned long lastListenMs = 0;
unsigned long lastVolStepMs = 0;

// ---------- Utility: read peak-to-peak over a time window ----------
int readMicP2P(unsigned long window_ms) {
  int minV = 1023;
  int maxV = 0;
  unsigned long start = millis();
  while (millis() - start < window_ms) {
    int v = analogRead(MIC_PIN);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  return maxV - minV; // peak-to-peak amplitude
}

// ---------- Utility: map with clamp ----------
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

void setup() {
  Serial.begin(9600);
  mySerial.begin(9600);

  for (int i = 0; i < 3; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
  }

  if (!myDFPlayer.begin(mySerial)) {
    Serial.println("DFPlayer Mini not detected!");
    while (true);
  }

  Serial.println("DFPlayer Mini ready!");
  myDFPlayer.volume(currentVolume);   // 0..30
  Serial.println("Playing File 1");
  myDFPlayer.play(currentTrack);      // Start first MP3

  // Initialize ambient baseline with a brief clean read (pause -> sample -> resume)
  delay(400);
  myDFPlayer.pause();
  delay(2); // settle after pause
  int baseP2P = readMicP2P(LISTEN_WINDOW_MS);
  ambientEMA = baseP2P;
  myDFPlayer.start(); // resume
  lastListenMs = millis();
  lastVolStepMs = millis();

  Serial.println("Auto-volume initialized");
}

void loop() {
  // Handle buttons: -1=just released, 0=not pressed, 1=held down, 2=just pressed
  buttonHandler();

  // Manual volume control via buttons (left/right buttons)
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

  // Optional: middle button as a simple play/pause toggle on "just pressed"
  if (buttons[1] == 2) {
    // Simple play/pause behavior; DFPlayer has no direct state query, so toggle naively
    // Pause briefly and resume; if stopped, re-play current track.
    myDFPlayer.pause();
    delay(150);
    myDFPlayer.start();
    Serial.println("Play/Pause toggle");
  }

  // Auto-volume logic: periodic "listen window"
  unsigned long now = millis();
  if (now - lastListenMs >= LISTEN_PERIOD_MS) {
    // Pause playback to avoid measuring self-audio
    myDFPlayer.pause();
    delay(2); // small settle time

    // Measure ambient via peak-to-peak during a short window
    int p2p = readMicP2P(LISTEN_WINDOW_MS);

    // Resume playback immediately
    myDFPlayer.start();

    // EMA smoothing with deadband to reduce jitter
    float diff = (float)p2p - ambientEMA;
    if (diff < -AMBIENT_DEADBAND || diff > AMBIENT_DEADBAND) {
      ambientEMA = (1.0f - AMBIENT_ALPHA) * ambientEMA + AMBIENT_ALPHA * (float)p2p;
    }
    lastListenMs = now;

    // Map ambient P2P to a target volume
    int targetVol = mapClamped((int)ambientEMA,
                               P2P_MIN_EXPECTED, P2P_MAX_EXPECTED,
                               MIN_VOL, MAX_VOL);
    if (targetVol > dfpMaxVolume) targetVol = dfpMaxVolume;
    if (targetVol < 0) targetVol = 0;

    // Step slowly toward target to avoid audible pumping
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

    // Debug prints
    Serial.print("P2P=");
    Serial.print(p2p);
    Serial.print(" EMA=");
    Serial.print(ambientEMA, 1);
    Serial.print(" targetVol=");
    Serial.print(targetVol);
    Serial.print(" currVol=");
    Serial.println(currentVolume);
  }

  delay(50);
}

// ---------- Button handler (debounced state machine) ----------
void buttonHandler() { // -1=just released, 0=not pressed, 1=held down, 2=just pressed
  for (int i = 0; i < 3; i++) {
    int b = !digitalRead(buttonPins[i]); // active LOW
    if (b == HIGH) { // button held
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
        buttons[i] = 0; // not pressed
      }
    }
  }

  for (int i = 0; i < 3; i++) {
    prevButtons[i] = (buttons[i] > 0) ? 1 : 0;
  }
}
