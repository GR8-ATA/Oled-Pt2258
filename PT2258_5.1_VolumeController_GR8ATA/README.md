# Digital 5.1 Channel Volume Controller — PT2258 + Arduino Nano

> **This is the "GR8-ATA" custom-splash variant.** It is identical to the
> default sketch except the boot screen shows **"GR8-ATA"**. Upload this one for
> the custom logo, or the sibling folder for the default splash.

Full firmware for a 6-channel (5.1) digital volume controller built around the
**PT2258** electronic volume IC, an **Arduino Nano**, and an **SSD1306 128x64
OLED**. Control via 5 push buttons or an **IR remote** (NEC). A 5-stage voltage
divider on `A0` shows a text label (e.g. input source), and a **relay** switches
the audio stage power.

---

## Features

- Independent per-channel attenuation (**0 … −79 dB**) for all 6 channels
- **Master** volume (global attenuation) via the PT2258 master register
- Channel select cycles: `MASTER → FL → FR → CENTER → SUB → RL → RR`
- **Mute / Unmute** (all channels)
- **Power** button toggles the output relay + soft mute on power-down
- **IR remote** control of master + every channel (NEC), with:
  - editable placeholder codes for a common remote
  - a **serial learning mode** to capture *your* remote's codes
- **5-stage voltage divider** → text label on the OLED
- **Rotary encoder** (optional): rotate = volume, press = channel select
- **Tone presets**: FLAT / MOVIE / MUSIC / NIGHT (one tap sets all 6 channels)
- **Startup splash** screen on boot
- **Auto standby**: relay powers off after a set idle time
- Settings saved to **EEPROM** and restored on power-up

---

## Two versions in this project

| Folder | Startup splash |
|---|---|
| `PT2258_5.1_VolumeController/` | Default splash ("5.1 CH / DIGITAL VOLUME") |
| `PT2258_5.1_VolumeController_GR8ATA/` | Custom **"GR8-ATA"** splash |

Both are identical otherwise — pick whichever splash you prefer and upload it.

---

## Bill of Materials

| Part | Notes |
|---|---|
| Arduino Nano (ATmega328P) | 5V logic |
| PT2258 | 6-channel I2C volume IC |
| SSD1306 128x64 OLED | I2C, addr `0x3C` |
| IR receiver | TSOP1738 / VS1838B (38 kHz) |
| 5 × push buttons | to GND (internal pull-ups used) |
| Relay module | active-HIGH (configurable) |
| Resistor ladder | 5-stage divider into `A0` |

---

## Wiring (Arduino Nano)

| Signal | Nano pin |
|---|---|
| I2C SDA (PT2258 + OLED) | **A4** |
| I2C SCL (PT2258 + OLED) | **A5** |
| IR receiver OUT | **D4** |
| Button — Volume Up | **D3** → GND |
| Button — Volume Down | **D2** → GND |
| Button — Channel Select | **D5** → GND |
| Button — Mute | **D6** → GND |
| Button — Power | **D7** → GND |
| Relay IN | **D8** |
| Voltage divider tap | **A0** |
| Rotary encoder CLK / DT / SW | **D9** / **D10** / **D11** |

> Buttons use `INPUT_PULLUP` → wire each button between the pin and **GND**.
> Relay defaults to **active HIGH**; set `RELAY_ACTIVE_HIGH false` in the sketch
> if your board is active-LOW.

### PT2258 I2C address
`CODE1` and `CODE2` tied **low** → write address `0x88` / read `0x89`
→ 7-bit `0x44` (as used in the sketch). If you strap them differently, update
`PT2258_ADDR`.

### PT2258 audio notes
- Requires a stable I2C clock; sketch uses **100 kHz**.
- Needs **≥300 ms** after power-up before it accepts commands (handled in code).
- The **clear register** (`0xC0`) is written once on init (handled in code).

---

## Required Libraries (Arduino Library Manager)

1. **Adafruit GFX Library**
2. **Adafruit SSD1306**
3. **IRremote** (v4.x, by Armin Joachimsmeyer)

Board: *Arduino Nano*, Processor: *ATmega328P* (use *Old Bootloader* if your
clone needs it).

---

## Using the IR Remote

The sketch ships with **placeholder NEC codes** for a generic remote. To use
your own remote:

1. Upload the sketch, open **Serial Monitor @ 115200 baud**.
2. Send `L` to enter **IR learn mode**.
3. Press each remote key — the monitor prints lines like:
   ```
   [LEARN] addr=0x0  cmd=0x45  proto=NEC
   ```
4. Copy the `cmd` values into the `#define IR_...` block near the top of the
   sketch, then re-upload.

### Default IR map (placeholders)

| Function | `cmd` |
|---|---|
| Power | `0x45` |
| Mute | `0x47` |
| Volume Up | `0x40` |
| Volume Down | `0x19` |
| Next channel | `0x09` |
| Prev channel | `0x07` |
| Cycle tone preset | `0x16` |
| Select FL / FR / CENTER | `0x0C` / `0x18` / `0x5E` |
| Select SUB / RL / RR | `0x08` / `0x1C` / `0x5A` |
| Select MASTER | `0x42` |

---

## Serial Console Commands (115200 baud)

| Key | Action |
|---|---|
| `L` | toggle IR learn mode |
| `P` | toggle power / relay |
| `M` | toggle mute |
| `+` / `-` | volume up / down (selected) |
| `>` / `<` | next / previous channel |
| `T` | cycle tone preset (FLAT/MOVIE/MUSIC/NIGHT) |
| `?` | help |

---

## Tone presets

Four presets set all six channel attenuations at once. Cycle them with the IR
preset key (`0x16`) or serial `T`; the active preset shows top-right on the OLED.

| Preset | Character |
|---|---|
| FLAT | all channels equal |
| MOVIE | loud center + sub, softer surrounds |
| MUSIC | fronts forward, center pulled back |
| NIGHT | low sub, gentle overall level |

Edit the exact dB values in the `PRESET_ATTEN[][]` table in the sketch.

## Rotary encoder (optional)

Wire an encoder to D9 (CLK), D10 (DT), D11 (SW). Rotating adjusts the volume of
the selected channel; pressing cycles the channel selection. It works alongside
the push buttons — you can use either or both.

## Auto standby

The relay powers off automatically after `STANDBY_MINUTES` (default **30**) with
no button/IR/encoder/serial activity. Set `STANDBY_MINUTES 0` to disable.

---

## Customizing the 5-Stage Voltage Divider

Edit these in the sketch:

```cpp
const char* VDIV_LABELS[5]     = { "AUX", "BLUETOOTH", "USB", "OPTICAL", "COAXIAL" };
const int   VDIV_THRESHOLD[5]  = { 102, 307, 512, 717, 1023 };
```

`A0` is read (0–1023) and mapped to the first band whose threshold it falls
under. Adjust thresholds to match your resistor ladder, and rename the labels
to whatever the 5 levels represent for you.

---

## How attenuation / dB works

The PT2258 attenuates the signal, so **0 dB = loudest** and **−79 dB =
quietest**. Internally each channel stores an attenuation `0..79`; the OLED
shows it as a negative dB value with a bar (full = 0 dB, empty = −79 dB).

---

## First-run defaults

- All channels: **−20 dB**
- Master: **−30 dB**
- Muted: no, Power: off (toggle with the Power button / `P`)

Change EEPROM defaults in `loadSettings()`. Bump `EEPROM_MAGIC` to force a
reset of stored settings.
