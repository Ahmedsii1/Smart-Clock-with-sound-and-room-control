# ESP32 Smart Clock — Scheduled Audio & AC Fan Control

An ESP32-based smart clock that runs a daily routine on its own: at a set time it switches off a 220 V AC fan, plays a sequence of audio files through a speaker, and turns the fan back on after a set period. Timekeeping is handled by a DS3231 real-time clock with battery backup, so the schedule survives power cuts and never needs Wi-Fi.

![Project photo](images/smart_clock.jpg)
<!-- Add a photo of the finished build at images/smart_clock.jpg -->

---

## Features

- **Daily scheduled event** — at a fixed time (default 05:30), the fan turns off and three sounds play back-to-back.
- **Automatic fan restore** — the fan turns back on after a configurable period (default 20 minutes).
- **Sequential audio playback** — each sound starts as soon as the previous one finishes, using the DFPlayer Mini's "track finished" signal, with a backup timer in case the signal is missed.
- **Accurate offline timekeeping** — DS3231 RTC (±2 ppm, about 1 minute drift per year) with coin-cell backup.
- **Reboot-safe** — the fan state is recalculated from the RTC every second, so a reset or power cut never leaves the fan stuck off or replays the sounds.
- **Fail-safe fan wiring** — the fan is wired to the relay's normally-closed contact, so it keeps running if the controller loses power.
- **Serial Monitor control** — play any sound, play all, stop, change volume, view status, and set the clock over USB.
- **Low power settings** — Wi-Fi and Bluetooth disabled, CPU clocked down to 80 MHz.

---

## Hardware

| Component | Notes |
|---|---|
| ESP32 DevKit (30-pin) | Main controller |
| DS3231 RTC module | With CR2032 backup cell |
| DFPlayer Mini | MP3 player module, reads from microSD |
| Speaker | 4–8 Ω, connected to SPK_1 / SPK_2 |
| microSD card | ≤ 32 GB, FAT32 |
| 5 V 1-channel relay module | Songle SRD-05VDC-SL-C, active-LOW |
| 220 V AC fan | Switched through the relay |
| 1 kΩ resistor | In series with the DFPlayer RX line |
| 220 Ω resistor | In series with the relay IN line |
| 470 µF capacitor | Across DFPlayer VCC/GND, prevents resets from audio current spikes |
| 5 V USB phone charger | Powers the ESP32 through its USB port |

---

## Wiring

All signal connections use the right-side header of the 30-pin ESP32. When the board is powered over USB, its **VIN** pin outputs about 5 V, which feeds the DFPlayer and relay.

| ESP32 pin | Connects to |
|---|---|
| VIN | 5 V rail (DFPlayer VCC, relay VCC) |
| GND | Common ground rail |
| 3V3 | DS3231 VCC |
| GPIO21 (SDA) | DS3231 SDA |
| GPIO22 (SCL) | DS3231 SCL |
| GPIO16 (RX2) | DFPlayer TX |
| GPIO17 (TX2) | 1 kΩ → DFPlayer RX |
| GPIO23 | 220 Ω → Relay IN |

**DFPlayer Mini:** SPK_1 and SPK_2 go to the speaker (neither goes to GND). A 470 µF capacitor sits across VCC and GND.

**Mains side (220 V):**

| From | To |
|---|---|
| Plug live (brown) | Relay COM |
| Relay NC | Fan live |
| Plug neutral (blue) | Fan neutral (direct) |
| Plug earth | Fan earth (if present) |

> ⚠️ **Mains voltage is dangerous.** Switch the live wire only, keep all 220 V wiring off the breadboard, insulate the underside of the relay module, and enclose the build before plugging it in.

### A note on driving the relay

The relay module is active-LOW and powered from 5 V. It only turns off when its IN pin is close to 5 V, so the ESP32's 3.3 V HIGH output keeps it permanently on. Instead of an extra transistor, this build switches the pin mode:

- **Relay ON:** pin set as `OUTPUT` and driven `LOW`.
- **Relay OFF:** pin set as `INPUT` (high impedance), letting the module pull IN up by itself.

The 220 Ω series resistor limits the current that can flow into the ESP32 pin while it floats (the module holds IN at about 4.3 V). A cleaner long-term solution is a small NPN transistor (e.g. 2N2222 or BC547) between GPIO23 and the relay IN.

---

## SD card setup

Format the card as **FAT32** and arrange the files like this:

```
SD card (root)
└── mp3
    ├── 0001.mp3
    ├── 0002.mp3
    └── 0003.mp3
```

File names must be four digits. The DFPlayer plays files by these numbers, not by their original names. Files must be real MP3s (renaming `.opus`, `.ogg` or `.m4a` files will not work).

---

## Software setup

1. Install the **Arduino IDE**.
2. In **Boards Manager**, install **esp32 by Espressif Systems** and select **ESP32 Dev Module**.
3. In **Library Manager**, install:
   - **RTClib** by Adafruit (accept the Adafruit BusIO dependency)
   - **DFRobotDFPlayerMini** by DFRobot
4. Open `smart_clock/smart_clock.ino`, adjust the settings, and upload.
5. Open the Serial Monitor at **115200 baud** with line ending **Newline**, and set the clock with the `S` command.

### Settings

Located at the top of the sketch:

| Constant | Default | Meaning |
|---|---|---|
| `EVENT_HOUR` / `EVENT_MINUTE` | 5 / 30 | Daily event time (24-hour format) |
| `FAN_OFF_MINUTES` | 20 | How long the fan stays off |
| `NUM_SOUNDS` | 3 | Number of sounds played at the event and by `A` |
| `TRACK_BACKUP_SECONDS` | 90 | Fallback delay before the next sound if "finished" is not received |
| `SOUND_VOLUME` | 30 | DFPlayer volume (0–30) |

---

## Serial commands

| Command | Action |
|---|---|
| `P 1` | Play sound 1 (any number: `P 2`, `P 3`, …) |
| `A` | Play all sounds in order |
| `X` | Stop playback |
| `V 20` | Set volume 0–30 until the next restart (`V` alone shows it) |
| `T` | Show time, fan state, volume and playback status |
| `S 2026-10-16 17:50:00` | Set the RTC date and time |
| `H` | Show the command list |

Commands are not case-sensitive.

---

## How it works

Every second, the ESP32 reads the time from the DS3231 and checks whether it falls inside the "fan off" window (from the event time until `FAN_OFF_MINUTES` later, handling midnight wrap-around). The fan state is derived from that check rather than stored, so after any reset the board immediately returns to the correct state.

When the window starts, the relay is energized (opening the NC contact and stopping the fan), and audio playback begins. The DFPlayer reports when each track finishes, and the next track is started at that moment. A backup timer advances the sequence if the "finished" message is lost. When the window ends, the relay is released and the fan restarts.

---

## Possible improvements

- Replace the floating-pin relay drive with an NPN transistor.
- Sync the clock automatically over Wi-Fi (NTP).
- Set the schedule from a phone through a small web page hosted on the ESP32.
- Add multiple events or different schedules for different weekdays.

---

## Repository structure

```
├── smart_clock/
│   └── smart_clock.ino
├── images/
│   └── smart_clock.jpg
└── README.md
```

---

## Author

Built by [Your Name](https://github.com/your-username).

## License

This project is released under the MIT License. See `LICENSE` for details.
