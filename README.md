# MegaWifi MOD Player

A CD-player-style ProTracker MOD music player for the Sega Genesis,
using the MegaWifi ESP32-C3 cartridge for audio decode and PWM output.

```
┌─────────────────────────────────────────────────────────────┐
│  ====== MEGAWIFI MOD PLAYER ======                          │
│                                                             │
│  FW: v1.5-ec3                                               │
│  > PLAYING    Pat 03/2A  Row 18/3F                          │
│                                                             │
│  CH1    CH2    CH3    CH4                                   │
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
│  ~~~ Space Debris by Captain (1993) ~~~ 4-channel ~~~       │
│  ════════════════════════════════════                       │
└─────────────────────────────────────────────────────────────┘
```

## Architecture

```
┌──────────────────────┐          ┌─────────────────────────────┐
│   Sega Genesis 68k   │  UART   │   ESP32-C3 (MegaWifi)        │
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
│  │ Marquee        │  │         │  │ Per-ch vol + pan       │  │
│  │ Pixel scroll   │  │         │  │ 44 kHz ISR             │  │
│  └────────────────┘  │         │  └──────────┬─────────────┘  │
│                      │         │             │                │
│  SGDK + mw-api       │         │  ┌──────────▼─────────────┐  │
│                      │         │  │ PWM Audio Driver       │  │
└──────────────────────┘         │  │ 12-bit LEDC @ 19.5kHz  │  │
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

## Audio Subsystem

### Mixer — 8 Uniform Mono Channels

All 8 channels (0–7) are identical with independent:
- **Volume**: 0–255
- **Pan**: 0 (hard left) → 128 (center) → 255 (hard right)

Two channel types:
- **SAMPLE**: 8-bit unsigned PCM with fixed-point sample rate conversion,
  optional linear interpolation, looping
- **STREAM**: ring buffer fed by a decoder task (MOD or MP3)

By convention, the MOD/MP3 player uses CH6 (left) and CH7 (right).
Channels 0–5 are available for sound effects.

### Decoders

| Decoder | Format | Type | Notes |
|---------|--------|------|-------|
| **micromod** | ProTracker MOD (4/8 ch) | Fixed-point | 669 lines, BSD-3 |
| **Helix** | MP3 (Layer III) | Fixed-point | RPSL license |

minimp3 was evaluated but rejected — float-based, too slow on the
FPU-less ESP32-C3 RISC-V core.

### PWM Output

| Parameter | Value |
|-----------|-------|
| GPIO L / R | 4 / 5 |
| Resolution | 12-bit (4096 levels) |
| PWM frequency | 19,531 Hz (APB 80 MHz / 4096) |
| Sample rate | 44,053 Hz (GPTimer 10 MHz / 227) |
| Midscale (silence) | 2048 |
| RC filter | R=2.2 kΩ, C=10 nF → f_c ≈ 7.2 kHz |

## Command Protocol

Genesis communicates with the ESP32-C3 over the MegaWifi LSD
protocol on control channel 0. Commands are standard MegaWifi
`mw_cmd` packets.

### Audio Commands

| Command | ID | Direction | Payload | Description |
|---------|---:|-----------|---------|-------------|
| `MW_CMD_AUD_PLAY` | 59 | Gen→ESP | — | Play embedded MOD (instant restart) |
| `MW_CMD_AUD_STOP` | 60 | Gen→ESP | — | Stop playback |
| `MW_CMD_AUD_PAUSE` | 61 | Gen→ESP | — | Pause (ring buffer preserved) |
| `MW_CMD_AUD_RESUME` | 62 | Gen→ESP | — | Resume from pause |
| `MW_CMD_AUD_STATUS` | 63 | Gen←ESP | 8 bytes | Packed status (see below) |
| `MW_CMD_AUD_LIST` | 64 | Gen←ESP | — | Reserved for track listing |
| `MW_CMD_AUD_VOL` | 65 | Gen→ESP | 1 byte (vol) | Set master volume 0–255 |

### Packed Status Response (8 bytes = 2 × u32, big-endian)

```
Word 0 — VU + state:
  [31:30]  state       0=stopped, 1=playing, 2=paused
  [29:23]  CH4 amp     0–127 (maps directly to pixel height)
  [22:16]  CH3 amp     0–127
  [15:9]   CH2 amp     0–127
  [8:2]    CH1 amp     0–127
  [1:0]    reserved

Word 1 — position:
  [31:24]  pattern     current pattern in sequence
  [23:16]  row         current row (0–63)
  [15:8]   song_len    total patterns
  [7:0]    master_vol  current master volume
```

Amplitudes are tracked at the 44 kHz sample rate inside micromod's
inner render loop (not the volume envelope). Peak-and-clear on read.
Scaled to 0–127 to map directly to VU pixel height without math on
the 68000 side.

## Genesis Application

Built with SGDK and the MegaWifi API (`mw-api`).

### Controls

| Button | Action |
|--------|--------|
| **A** | Play (instant restart — rapid press = rapid restart) |
| **B** | Stop |
| **C** | Pause / Resume toggle |
| **Up** | Volume up (+10) |
| **Down** | Volume down (−10) |
| **Start** | Track select (reserved) |

### Display

- **Status line**: state + pattern/row counter (flicker-free padded writes)
- **VU meters**: 68 sprites, 4 channels × 16 rows + 4 peak hold lines.
  Green (bottom 60%) → Yellow (60–80%) → Red (top 20%).
  Ballistic IIR smoothing: fast attack (75%/frame), slow decay (1/8+1/frame).
  Peak hold with gravity fall.
- **Volume bar**: solid blue tiles on BG_A
- **Marquee**: pixel-smooth scroll via VDP HSCROLL_TILE mode (1 px/frame)

### Build

```sh
cd ~/MegaWifi/mod_player
make
# ROM: out/mod_player.bin
```

Requires:
- `m68k-elf-gcc` cross toolchain
- SGDK at `~/sgdk`
- MegaWifi API in SGDK (`ext/mw/`)

### Firmware

The ESP32-C3 firmware lives in a separate repository:
[mw-fw-rtos](https://github.com/mikewolak/mw-fw-rtos)

Build with ESP-IDF v5.x:
```sh
cd ~/MegaWifi/mw-fw-rtos
source ~/esp/esp-idf/export.sh
# Enable audio in menuconfig: MegaWiFi options → Enable PWM audio
idf.py build
```

## Required Hardware Modification

**The MegaWifi Rev B board requires a capacitor swap for audio to work.**

The stock board has C16 and C17 = 1 µF on the PWM audio output path
(wifi.kicad_sch). This creates a 72 Hz low-pass filter that crushes
all audio content — music is inaudible (−23 dB at 1 kHz).

### Fix: Replace C16 and C17

| Component | Stock | Required | Footprint |
|-----------|-------|----------|-----------|
| C16 | 1 µF | **10 nF** | 0805 SMD |
| C17 | 1 µF | **10 nF** | 0805 SMD |

These are on the WiFi sub-board (wifi.kicad_sch), in the RC filter
between the ESP32-C3 PWM outputs and the Genesis cartridge audio bus.

### Filter characteristics after mod

```
R = 2.2 kΩ (R11/R12, unchanged)
C = 10 nF (C16/C17, replaced)

Cutoff:  f_c = 1/(2π × 2200 × 10e-9) = 7,234 Hz
```

| Frequency | Attenuation |
|-----------|-------------|
| 1 kHz | −2.8 dB |
| 5 kHz | −8.3 dB |
| 7.2 kHz | −3.0 dB (cutoff) |
| 19.5 kHz (PWM) | −8.6 dB |

The 7.2 kHz cutoff passes most music content. The 19.5 kHz PWM
switching frequency gets ~9 dB of attenuation — adequate when
the Genesis is powered from its own PSU (not USB-C).

### Alternative cap values

| Cap | Cutoff | PWM attenuation | Character |
|-----|--------|-----------------|-----------|
| 4.7 nF | 15.4 kHz | −2.1 dB | Maximum treble, some PWM whine |
| **10 nF** | **7.2 kHz** | **−8.6 dB** | **Recommended balance** |
| 22 nF | 3.3 kHz | −15.4 dB | Warmer, better PWM filtering |
| 47 nF | 1.5 kHz | −22.3 dB | Very warm, significant rolloff |

### Power supply note

USB-C power through the MegaWifi cartridge injects significant
switching noise into the Genesis audio chain. **Always use the
Genesis console's own power supply** for clean audio.

## Known Issues

- VU meter sprites have minor flicker during playback due to
  `mw_command()` blocking across frame boundaries. The poll's
  internal `TSK_superPend()` causes SGDK VSync processing that
  disrupts the VDP write/sprite update timing.
- Audio has a 7.2 kHz low-pass rolloff from the RC filter
  (R=2.2 kΩ, C=10 nF). Acceptable for game audio and tracker music.
- Model 2 Genesis has inherent audio noise from the ASIC mixer.
  USB-C power makes it worse — use the Genesis PSU.

## License

- Genesis application: MIT
- micromod: BSD-3-Clause (Martin Cameron)
- Helix MP3 decoder: RPSL (RealNetworks)
- MegaWifi firmware: GPL-3.0 (upstream by doragasu)

---

*March 2026 — Mike Wolak*
