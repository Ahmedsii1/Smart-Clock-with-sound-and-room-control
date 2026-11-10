/*
  ============================================================
  ESP32 Smart Clock - FINAL (with Serial commands)
  ------------------------------------------------------------
  Every day at 05:30:
    - fan turns OFF (relay ON -> NC contact opens)
    - sound 1 plays, then sound 2, then sound 3
      (each starts when the previous one finishes;
       backup: next sound starts after TRACK_BACKUP_SECONDS)
  At 05:50 (after FAN_OFF_MINUTES):
    - fan turns back ON (relay OFF -> NC closes)

  Serial Monitor commands (115200 baud, line ending "Newline"):
    P 1                    play sound 1 (any number: P 2, P 3 ...)
    A                      play all sounds 1 -> NUM_SOUNDS
    X                      stop sound
    V 20                   set volume 0-30 (V alone shows volume)
    T                      show time, fan, volume, playing
    S 2026-10-16 17:50:00  set the clock
    H                      help

  Wiring:
    DS3231   : VCC -> 3V3, GND -> GND, SDA -> D21, SCL -> D22
    DFPlayer : VCC -> 5V rail, GND -> GND, TX -> GPIO16,
               RX -> 1k -> GPIO17, SPK_1 / SPK_2 -> speaker
    Relay    : VCC -> 5V rail, GND -> GND, IN <- 220 ohm <- D23
    Fan      : on relay NC + COM
    SD card  : /mp3/0001.mp3, /mp3/0002.mp3, /mp3/0003.mp3
  ============================================================
*/

#include <Wire.h>
#include <WiFi.h>
#include <RTClib.h>
#include <DFRobotDFPlayerMini.h>

// ===================== SETTINGS =====================
constexpr int EVENT_HOUR           = 5;    // 05:30 (use 17 for 5:30 PM)
constexpr int EVENT_MINUTE         = 30;
constexpr int FAN_OFF_MINUTES      = 20;   // fan stays OFF for 20 minutes
constexpr int NUM_SOUNDS           = 3;    // event and "A" play 0001 -> 0003
constexpr int TRACK_BACKUP_SECONDS = 90;   // backup: next sound starts after 1.5 min
                                           // (must be LONGER than each sound except the last)
constexpr int SOUND_VOLUME         = 30;   // 0-30, volume after every restart

// ===================== PINS =====================
constexpr int PIN_SDA   = 21;
constexpr int PIN_SCL   = 22;
constexpr int PIN_DF_RX = 16;   // ESP32 RX2 <- DFPlayer TX
constexpr int PIN_DF_TX = 17;   // ESP32 TX2 -> 1k -> DFPlayer RX
constexpr int PIN_RELAY = 23;   // -> 220 ohm -> relay IN

static_assert(EVENT_HOUR >= 0 && EVENT_HOUR < 24, "EVENT_HOUR must be 0-23");
static_assert(EVENT_MINUTE >= 0 && EVENT_MINUTE < 60, "EVENT_MINUTE must be 0-59");
static_assert(FAN_OFF_MINUTES > 0 && FAN_OFF_MINUTES < 1440, "FAN_OFF_MINUTES must be 1-1439");
static_assert(SOUND_VOLUME >= 0 && SOUND_VOLUME <= 30, "SOUND_VOLUME must be 0-30");

RTC_DS3231 rtc;
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini player;

bool     playerReady    = false;
bool     wasInWindow    = false;
int      currentTrack   = 0;     // 0 = nothing playing
int      sequenceEnd    = 0;     // last sound of the current sequence
uint32_t trackStartedAt = 0;
int      currentVolume  = SOUND_VOLUME;

// ===================== RELAY / FAN =====================
// Relay ON  = pin outputs LOW
// Relay OFF = pin floats (module pulls IN up by itself)
void relayOn()  { pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, LOW); }
void relayOff() { pinMode(PIN_RELAY, INPUT); }

// Fan on NC: relay OFF = fan ON, relay ON = fan OFF
void fanOn()  { relayOff(); }
void fanOff() { relayOn(); }

// ===================== SOUND =====================
void startTrack(int n) {
  if (!playerReady) return;
  player.playMp3Folder(n);
  currentTrack   = n;
  trackStartedAt = millis();
  Serial.printf("  Playing sound %d (/mp3/%04d.mp3)\n", n, n);
}

// Plays sounds first..last, one after another
void playSequence(int first, int last) {
  if (!playerReady) {
    Serial.println("  DFPlayer not ready - cannot play");
    return;
  }
  sequenceEnd = last;
  startTrack(first);
}

void stopSound() {
  if (playerReady) player.stop();
  currentTrack = 0;
  sequenceEnd  = 0;
  Serial.println("  Sound stopped");
}

void nextOrFinish() {
  if (currentTrack < sequenceEnd) {
    startTrack(currentTrack + 1);
  } else {
    Serial.println("  Playback finished");
    currentTrack = 0;
    sequenceEnd  = 0;
  }
}

// Detects "sound finished" and errors from the DFPlayer
void handlePlayer() {
  if (!playerReady || !player.available()) return;

  uint8_t type  = player.readType();
  int     value = player.read();

  if (type == DFPlayerPlayFinished && currentTrack > 0) {
    // The DFPlayer often sends "finished" twice - ignore the repeat
    if (millis() - trackStartedAt < 1000) return;
    Serial.printf("  Sound %d finished\n", currentTrack);
    nextOrFinish();
  }
  else if (type == DFPlayerError) {
    if (value == FileMismatch || value == FileIndexOut) {
      Serial.printf("  Sound %d not found on SD card (/mp3/%04d.mp3) - skipping\n",
                    currentTrack, currentTrack);
    } else {
      Serial.printf("  DFPlayer error code %d - skipping\n", value);
    }
    if (currentTrack > 0) nextOrFinish();
  }
}

// Backup: if "finished" never arrives, move on after TRACK_BACKUP_SECONDS
// (not used for the last sound of a sequence - it just plays to the end)
void checkBackupTimer() {
  if (currentTrack == 0 || currentTrack >= sequenceEnd) return;
  if (millis() - trackStartedAt >= TRACK_BACKUP_SECONDS * 1000UL) {
    Serial.printf("  No 'finished' signal after %d s - starting next sound\n",
                  TRACK_BACKUP_SECONDS);
    nextOrFinish();
  }
}

// ===================== HELPERS =====================
bool inOffWindow(const DateTime& t) {
  int nowMin   = t.hour() * 60 + t.minute();
  int startMin = EVENT_HOUR * 60 + EVENT_MINUTE;
  int elapsed  = (nowMin - startMin + 1440) % 1440;
  return elapsed < FAN_OFF_MINUTES;
}

bool timeValid(const DateTime& t) {
  return t.year() >= 2024 && t.year() <= 2099 &&
         t.hour() < 24 && t.minute() < 60 && t.second() < 60;
}

void printTime(const DateTime& t) {
  Serial.printf("%04d-%02d-%02d %02d:%02d:%02d",
                t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
}

// ===================== SERIAL COMMANDS =====================
void printHelp() {
  Serial.println();
  Serial.println("=============== COMMANDS ===============");
  Serial.println(" P 1                    play sound 1 (P 2, P 3 ...)");
  Serial.printf (" A                      play all sounds 1 -> %d\n", NUM_SOUNDS);
  Serial.println(" X                      stop sound");
  Serial.println(" V 20                   set volume 0-30 (V = show)");
  Serial.println(" T                      show status");
  Serial.println(" S 2026-10-16 17:50:00  set the clock");
  Serial.println(" H                      this help");
  Serial.println("========================================");
}

void printStatus() {
  DateTime now = rtc.now();
  Serial.print("  Time: ");
  printTime(now);
  Serial.printf("\n  Event: %02d:%02d, fan off for %d min\n",
                EVENT_HOUR, EVENT_MINUTE, FAN_OFF_MINUTES);
  Serial.printf("  Fan: %s\n", wasInWindow ? "OFF" : "ON");
  Serial.printf("  Volume: %d\n", currentVolume);
  if (currentTrack > 0) {
    Serial.printf("  Playing: sound %d (sequence ends at %d)\n", currentTrack, sequenceEnd);
  } else {
    Serial.println("  Playing: nothing");
  }
  Serial.printf("  DFPlayer: %s\n", playerReady ? "ready" : "NOT ready");
}

void handleSerialCommand() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  char   cmd = toupper(line[0]);
  String arg = line.substring(1);
  arg.trim();

  Serial.printf("> %s\n", line.c_str());

  switch (cmd) {
    case 'P': {
      int n = arg.toInt();
      if (n < 1 || n > 3000) {
        Serial.println("  Usage: P 1   (sound number 1 or higher)");
      } else {
        playSequence(n, n);
      }
      break;
    }

    case 'A':
      playSequence(1, NUM_SOUNDS);
      break;

    case 'X':
      stopSound();
      break;

    case 'V': {
      if (arg.length() == 0) {
        Serial.printf("  Volume: %d\n", currentVolume);
        break;
      }
      int v = arg.toInt();
      if (v < 0 || v > 30) {
        Serial.println("  Volume must be 0-30");
      } else {
        currentVolume = v;
        if (playerReady) player.volume(currentVolume);
        Serial.printf("  Volume set to %d (until restart)\n", currentVolume);
      }
      break;
    }

    case 'T':
      printStatus();
      break;

    case 'S': {
      int Y, M, D, h, m, sec;
      if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) == 6) {
        rtc.adjust(DateTime(Y, M, D, h, m, sec));
        Serial.println("  Time set");
      } else {
        Serial.println("  Format: S 2026-10-16 17:50:00");
      }
      break;
    }

    case 'H':
      printHelp();
      break;

    default:
      Serial.println("  Unknown command - type H for help");
  }
}

// ===================== SETUP =====================
void setup() {
  fanOn();   // safe default: relay OFF, fan ON

  Serial.begin(115200);
  Serial.setTimeout(200);   // short wait when reading a command line
  delay(500);
  Serial.println("\n=== ESP32 Smart Clock - FINAL ===");

  WiFi.mode(WIFI_OFF);
  btStop();
  setCpuFrequencyMhz(80);

  // ---- RTC ----
  Wire.begin(PIN_SDA, PIN_SCL);
  while (!rtc.begin()) {
    Serial.println("DS3231 not found - retrying...");
    delay(2000);
  }
  if (rtc.lostPower()) {
    Serial.println("WARNING: RTC time is not set. Type: S YYYY-MM-DD HH:MM:SS");
  }

  // ---- DFPlayer ----
  dfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);
  delay(2000);
  for (int i = 1; i <= 3 && !playerReady; i++) {
    playerReady = player.begin(dfSerial, true, true);
    if (!playerReady) {
      Serial.printf("DFPlayer not responding (try %d/3)\n", i);
      delay(1000);
    }
  }
  if (playerReady) {
    player.outputDevice(DFPLAYER_DEVICE_SD);
    delay(200);
    player.volume(currentVolume);
    delay(200);
    while (player.available()) { player.readType(); player.read(); }  // clear startup messages
    Serial.println("DFPlayer ready");
  } else {
    Serial.println("DFPlayer FAILED - fan control still works");
  }

  // ---- Start in the correct state (no sound at boot) ----
  DateTime now = rtc.now();
  if (timeValid(now)) {
    wasInWindow = inOffWindow(now);
    if (wasInWindow) fanOff(); else fanOn();
  }

  Serial.print("Now: ");
  printTime(now);
  Serial.printf("\nEvent at %02d:%02d, fan off for %d min, sounds 1 -> %d\n",
                EVENT_HOUR, EVENT_MINUTE, FAN_OFF_MINUTES, NUM_SOUNDS);
  Serial.printf("Fan is currently %s\n", wasInWindow ? "OFF" : "ON");
  printHelp();
}

// ===================== LOOP =====================
void loop() {
  handleSerialCommand();
  handlePlayer();
  checkBackupTimer();

  static uint32_t lastCheck = 0;
  static int printCounter = 0;

  if (millis() - lastCheck >= 1000) {
    lastCheck = millis();
    DateTime now = rtc.now();

    if (timeValid(now)) {
      bool inWindow = inOffWindow(now);

      if (inWindow && !wasInWindow) {
        printTime(now);
        Serial.println("  EVENT -> fan OFF");
        fanOff();
        delay(500);        // let the relay settle before the speaker starts
        playSequence(1, NUM_SOUNDS);
      }
      else if (!inWindow && wasInWindow) {
        printTime(now);
        Serial.println("  END -> fan ON");
        fanOn();
      }
      wasInWindow = inWindow;

      // Status line every 60 seconds (less clutter while typing commands)
      if (++printCounter >= 60) {
        printCounter = 0;
        printTime(now);
        Serial.println(inWindow ? "  fan: OFF" : "  fan: ON");
      }
    } else {
      Serial.println("Bad RTC read - skipping");
    }
  }

  delay(20);
}