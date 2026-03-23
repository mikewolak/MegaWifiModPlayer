# MegaWifi MOD Player

A CD-player-style ProTracker MOD music player for the Sega Genesis,
using the MegaWifi ESP32-C3 cartridge for audio decode, mixing,
reverb processing, and stereo PWM output.

```
┌─────────────────────────────────────────────────────────────┐
│  ====== MEGAWIFI MOD PLAYER ======                          │
│  Reverb: ON   [START] Settings                              │
│  FW: v1.5-ec3                                               │
│  > PLAYING    Pat 03/2A  Row 18/3F                          │
│                                                             │
│  CH1    CH2    CH3    CH4                                    │
│  ████   ██     ████   █                                     │
│  ████   ██     ████   █                                     │
│  ████   ██     ████                                         │
│  ████          ████                                         │
│  ████          ██                                           │
│  ██                                                         │
│                                                             │
│  Vol: ████████████████░░░░ 75%                              │
│                                                             │
│  [A]Play [B]Stop [C]Pause [^v]Vol                           │
│                                                             │
│  ~~~ Freeverb reverb plugin ~~~ Per-channel send/return ~~~ │
│  ════════════════════════════════════                        │
└─────────────────────────────────────────────────────────────┘
```

## Architecture

```
┌──────────────────────┐          ┌─────────────────────────────┐
│   Sega Genesis 68k   │  UART   │   ESP32-C3 (MegaWifi)       │
│                      │ 1.5Mbit │                              │
│  ┌────────────────┐  │  LSD    │  ┌────────────────────────┐  │
│  │ Transport UI   │──┼─────────┼─→│ MegaWifi FSM           │  │
│  │ A/B/C buttons  │  │  ch 0   │  │ MW_CMD_AUD_* handlers  │  │
│  └────────────────┘  │         │  └──────────┬─────────────┘  │
│                      │         │             │                │
│  ┌────────────────┐  │         │  ┌──────────▼─────────────┐  │
│  │ VU Meters      │←─┼─────────┼──│ MOD Player (micromod)  │  │
│  │ 68 sprites     │  │ packed  │  │ Fixed-point decode     │  │
│  │ 60 FPS         │  │ status  │  └──────────┬─────────────┘  │
│  └────────────────┘  │         │             │                │
│                      │         │  ┌──────────▼─────────────┐  │
│  ┌────────────────┐  │         │  │ 8-Channel Mixer        │  │
│  │ Reverb Control │  │         │  │ Per-ch vol + pan       │  │
│  │ START popup    │──┼─────────┼─→│ Per-ch reverb send     │  │
│  └────────────────┘  │         │  │ 44 kHz ISR             │  │
│                      │         │  └──┬───────┬─────────────┘  │
│  ┌────────────────┐  │         │     │       │                │
│  │ Marquee        │  │         │     │  ┌────▼─────────────┐  │
│  │ Pixel scroll   │  │         │     │  │ Freeverb         │  │
│  └────────────────┘  │         │     │  │ Send/Return bus  │  │
│                      │         │     │  │ Jezar algorithm   │  │
│  SGDK + mw-api      │         │     │  └────┬─────────────┘  │
│                      │         │     │       │                │
└──────────────────────┘         │  ┌──▼───────▼─────────────┐  │
                                 │  │ PWM Audio Driver       │  │
                                 │  │ 12-bit LEDC @ 19.5kHz  │  │
                                 │  │ GPIO4(L) GPIO5(R)      │  │
                                 │  └──────────┬─────────────┘  │
                                 │             │                │
                                 └─────────────┼────────────────┘
                                               │
                                          RC Filter
                                          (R=2.2k C=10nF)
                                               │
                                        Genesis Audio Bus
                                          (cart B8/B9)
```

## Mixer Signal Flow

```
CH0 ──vol──┬──pan──→ dry bus L/R ──┐
CH1 ──vol──┤                       │
 ...       ├──send──→ reverb bus ──┤
CH6 ──vol──┤            │          │
CH7 ──vol──┘            ▼          │
                    ┌────────┐     │
                    │Freeverb│     │
                    │8 comb  │     │
                    │4 allpas│     │
                    └───┬────┘     │
                        │          │
              return ←──┘          │
                 │                 │
                 ▼                 ▼
              mix L/R = dry + (return × level)
                        │
                   master vol
                        │
                  12-bit clamp
                        │
                    LEDC PWM
```

Each of the 8 channels independently controls:
- **Volume**: 0–255
- **Pan**: 0 (hard left) → 128 (center) → 255 (hard right)
- **Reverb send**: 0–255 (how much goes to the reverb bus)

The reverb runs continuously — when the MOD stops, the tail
decays naturally through the allpass/comb filter network.

## Reverb Engine

Jezar's Freeverb algorithm (Schroeder-Moorer topology), ported
from PaulStoffregen's Teensy Audio Library to pure C fixed-point.

- 8 parallel comb filters (delay + damped feedback)
- 4 series allpass filters (diffusion)
- All int16/int32 arithmetic, no float
- ~23 KB RAM for delay line buffers
- Runs per-sample at 44 kHz inside the mixer ISR

### Reverb Controls (Genesis START menu)

```
┌─────────────────────────────────────────┐
│ ---- REVERB CONTROL ---- START=close -- │
│                                         │
│ > Reverb:  [ON]  off                    │
│   Preset:  < CAVE >                     │
│   Mix:     ████████████░░░░░░░░  51%    │
│   Decay:   ████████████████░░░░  82%    │
│ ---- CHANNEL SENDS ----                 │
│   CH1:     ░░░░░░░░░░░░░░░░░░░░   0%   │
│   CH2:     ░░░░░░░░░░░░░░░░░░░░   0%   │
│   CH3:     ░░░░░░░░░░░░░░░░░░░░   0%   │
│   CH4:     ░░░░░░░░░░░░░░░░░░░░   0%   │
│   CH5:     ░░░░░░░░░░░░░░░░░░░░   0%   │
│   CH6:     ░░░░░░░░░░░░░░░░░░░░   0%   │
│   CH7:     ██████████░░░░░░░░░░  51%   │
│   CH8:     ██████████░░░░░░░░░░  51%   │
│ ─────────────────────────────────────── │
└─────────────────────────────────────────┘
```

| Control | Action |
|---------|--------|
| Up/Down | Navigate parameters |
| Left/Right | Adjust value (±10 per press) |
| A | Toggle bypass on/off |
| START | Close popup |

Presets map to Freeverb roomsize values:
- **Room**: small, short decay
- **Hall**: concert hall, medium decay
- **Plate**: dense, fast build
- **Cave**: long, dark tail (default)

## Command Protocol

Genesis communicates with the ESP32-C3 over the MegaWifi LSD
protocol on control channel 0.

### Audio Commands

| Command | ID | Direction | Payload | Description |
|---------|---:|-----------|---------|-------------|
| `MW_CMD_AUD_PLAY` | 59 | Gen→ESP | — | Play MOD (instant restart) |
| `MW_CMD_AUD_STOP` | 60 | Gen→ESP | — | Stop playback |
| `MW_CMD_AUD_PAUSE` | 61 | Gen→ESP | — | Pause |
| `MW_CMD_AUD_RESUME` | 62 | Gen→ESP | — | Resume |
| `MW_CMD_AUD_STATUS` | 63 | Gen←ESP | 8 bytes | Packed status |
| `MW_CMD_AUD_LIST` | 64 | Gen←ESP | — | Reserved |
| `MW_CMD_AUD_VOL` | 65 | Gen→ESP | 1 byte | Master volume 0–255 |

### Reverb Commands

| Command | ID | Direction | Payload | Description |
|---------|---:|-----------|---------|-------------|
| `MW_CMD_AUD_REVERB_ENABLE` | 66 | Gen→ESP | 1 byte | Enable/bypass (0 or 1) |
| `MW_CMD_AUD_REVERB_PRESET` | 67 | Gen→ESP | 1 byte | Preset (0–3) |
| `MW_CMD_AUD_REVERB_MIX` | 68 | Gen→ESP | 2 bytes | Return level (Q1.14) |
| `MW_CMD_AUD_REVERB_DECAY` | 69 | Gen→ESP | 2 bytes | Damping (Q1.14) |
| `MW_CMD_AUD_REVERB_SEND` | 70 | Gen→ESP | 2 bytes | ch + level (0–255) |

### Packed Status Response (8 bytes)

```
Word 0 — VU + state:
  [31:30]  state       0=stopped, 1=playing, 2=paused
  [29:23]  CH4 amp     0–127 (pixel height)
  [22:16]  CH3 amp     0–127
  [15:9]   CH2 amp     0–127
  [8:2]    CH1 amp     0–127
  [1:0]    reserved

Word 1 — position:
  [31:24]  pattern
  [23:16]  row (0–63)
  [15:8]   song_len
  [7:0]    master_vol
```

## PWM Output

| Parameter | Value |
|-----------|-------|
| GPIO L / R | 4 / 5 |
| Resolution | 12-bit (4096 levels) |
| PWM frequency | 19,531 Hz |
| Sample rate | 44,053 Hz |
| RC filter | R=2.2 kΩ, C=10 nF → f_c ≈ 7.2 kHz |

## Required Hardware Modification

**The MegaWifi Rev B board requires a capacitor swap for audio.**

The stock C16 and C17 = 1 µF creates a 72 Hz low-pass filter
that makes all audio inaudible.

### Fix: Replace C16 and C17

| Component | Stock | Required | Footprint |
|-----------|-------|----------|-----------|
| C16 | 1 µF | **10 nF** | 0805 SMD |
| C17 | 1 µF | **10 nF** | 0805 SMD |

### Filter after mod

```
f_c = 1/(2π × 2200 × 10e-9) = 7,234 Hz
```

| Cap | Cutoff | PWM attenuation | Notes |
|-----|--------|-----------------|-------|
| 4.7 nF | 15.4 kHz | −2 dB | Max treble, some PWM whine |
| **10 nF** | **7.2 kHz** | **−9 dB** | **Recommended** |
| 22 nF | 3.3 kHz | −15 dB | Warmer, better filtering |

**Always use the Genesis PSU, not USB-C power**, to avoid
switching noise on the audio bus.

## Genesis Controls

| Button | Action |
|--------|--------|
| **A** | Play (instant restart) |
| **B** | Stop |
| **C** | Pause / Resume |
| **Up/Down** | Master volume |
| **START** | Open/close reverb control panel |

## Build

### Genesis ROM

```sh
cd ~/MegaWifi/mod_player
make
# ROM: out/mod_player.bin
```

Requires: `m68k-elf-gcc`, SGDK at `~/sgdk`

### ESP32-C3 Firmware

```sh
cd ~/MegaWifi/mw-fw-rtos
source ~/esp/esp-idf/export.sh
# Enable audio: idf.py menuconfig → MegaWiFi options → Enable PWM audio
idf.py build
```

Firmware repo: [mw-fw-rtos](https://github.com/mikewolak/mw-fw-rtos)

## Known Issues

- VU meter sprites have minor flicker due to `mw_command()` blocking
  across frame boundaries during status polling.
- 7.2 kHz filter rolloff from the RC network. Acceptable for tracker
  music and game audio.
- Model 2 Genesis has inherent audio noise from the ASIC mixer.

## License

- Genesis application: MIT
- Freeverb: MIT (Paul Stoffregen / PJRC, original Jezar public domain)
- micromod: BSD-3-Clause (Martin Cameron)
- Helix MP3 decoder: RPSL (RealNetworks)
- MegaWifi firmware: GPL-3.0 (upstream by doragasu)

---

*March 2026 — Mike Wolak <mikewolak@gmail.com>*
