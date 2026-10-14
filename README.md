# Beacon — CW (Morse) Beacon for HF or VHF Radio

A Morse-code beacon firmware for the [Waveshare RP2040-Zero](https://www.waveshare.com/wiki/RP2040-Zero) board (Raspberry Pi RP2040), designed for keying a VHF FM/SSB transmitter.

The firmware generates an audio telegraph tone on one GPIO, controls transmitter PTT on another, transmits a configurable message stored in non-volatile (flash) memory, and repeats it with a configurable pause. All beacon parameters (tone frequency, message text, pause, keying speed, PTT polarity) are configurable at runtime over a USB serial console and persist across power cycles.

## Features

- **Telegraph tone output** — hardware PWM square wave, 100–5000 Hz (600/800 Hz typical for FM audio), default **800 Hz**
- **PTT control** — asserted at the start of the transmission and released after the end of the message, with configurable lead/tail delays and active level (0 or 1)
- **Morse message** — full A–Z, 0–9 and `. , ? / - =` encoding with standard element timing; speed set directly in WPM (default **20 WPM**)
- **Tone sequences** — an arbitrary chain of tone bursts ("freq:ms" pairs, `0` = silence), transmittable instead of (or after) the Morse message
- **Transmission modes** — `cw` (Morse only), `tones` (tone sequence only), or `both` (Morse, then tones) within one PTT
- **Non-volatile configuration** — stored in the last sector of the onboard flash; survives power-off and reflashing
- **Ready-to-flash firmware** — a prebuilt `.uf2` binary is included in [`firmware/`](firmware/), no toolchain required
- **USB console** — live configuration over the USB CDC serial port
- **Status LED** — onboard WS2812 (GP16): steady green in the pause, red blinking in sync with the keying during transmission

## Hardware

| Signal    | GPIO | Description                                      |
|-----------|------|--------------------------------------------------|
| Tone      | GP15 | Telegraph tone output → transmitter audio input  |
| PTT       | GP14 | PTT output → transmitter PTT line                |
| CW key    | GP7  | Key output → transmitter key input via transistor (keyed while the tone sounds) |
| WS2812    | GP16 | Onboard status LED (built into the board)        |

> Connect the tone output to the transmitter's audio/modulation input (use an attenuator or level shifter if required by your radio) and the PTT output to the transmitter's PTT input, sharing a common ground. The CW key output drives the transmitter's built-in keyer input through an external transistor (e.g. 2N2222 / IRLML6402 depending on key input polarity): GPIO → 1k resistor → transistor base, key line on the collector/emitter side. The key is closed while the tone sounds (dots/dashes in CW mode, active entries of the tone sequence).

![Waveshare RP2040-Zero pinout](pics/rp2040_zero_pinout.png)

## Flashing

A prebuilt firmware binary is included in the repository: [`firmware/Beacon-v0.1.uf2`](firmware/Beacon-v0.1.uf2). You do not need to build anything to flash it.

**Method 1 — BOOTSEL button:**

1. Hold the BOOT button and connect the board via USB.
2. The board appears as a USB mass-storage device (`RPI-RP2`).
3. Copy the firmware: `cp firmware/Beacon-v0.1.uf2 /run/media/$USER/RPI-RP2/`

**Method 2 — picotool (no button required):**

```bash
picotool load firmware/Beacon-v0.1.uf2 -fx
```

If the currently running firmware does not support USB reboot, use the BOOTSEL button method.

## Configuration commands (USB console)

Connect to the board's serial port (`/dev/ttyACM0` on Linux, e.g. `minicom -D /dev/ttyACM0` or `picocom /dev/ttyACM0`). Any baud rate works.

Type the command and press **Enter**. If your terminal can send no line ending, the line is executed automatically after a 5-second pause in typing.

| Command        | Description                                        |
|----------------|----------------------------------------------------|
| `msg <text>`   | Set message text (A–Z, 0–9, `. , ? / - =`)         |
| `tone <hz>`    | Set tone frequency, 100–5000 Hz                    |
| `pause <sec>`  | Pause between message repeats, 0–3600 s            |
| `wpm <5..60>`  | Keying speed, words per minute (dot = 1200 / wpm)  |
| `tones <f:d> ...` | Tone sequence: `freq:ms` pairs, freq 0–5000 Hz (`0` = silence), duration 10–10000 ms, up to 24 entries. Without arguments — show current sequence |
| `txmode cw\|tones\|both` | What to transmit: Morse only / tone sequence only / both |
| `txen <0\|1>`   | Enable/disable transmission (0 = beacon stays silent, e.g. while tuning) |
| `txen <0\|1>`   | Enable/disable transmission (0 = beacon stays silent, e.g. while tuning) |
| `ptt <0\|1>`   | PTT active level                                   |
| `save`         | Write current configuration to flash               |
| `show`         | Print current configuration                        |
| `default`      | Restore factory defaults                           |
| `help`         | Show command list                                  |

Changes take effect immediately; use `save` to make them permanent.

### Example session

```
msg CQ CQ CQ DE RA0SMS K
tone 600
pause 15
wpm 18
txmode both
tones 800:150 0:100 800:150 0:100 1200:150 0:500
save
```

## Building from source

If you want to modify the firmware, build it yourself. The project uses:

- **[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)** 2.3.1 (developed with this version; other 2.x versions likely work)
- **CMake** ≥ 3.13
- **ARM GCC cross-toolchain** — `arm-none-eabi-gcc` (the SDK 2.x toolchain package `15_2_Rel1` or any recent version)
- **Ninja** or GNU Make (Ninja is recommended)

### Installing the toolchain

**Fedora / RHEL:**

```bash
sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-newlib cmake ninja-build
git clone -b 2.3.1 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
export PICO_SDK_PATH=~/pico-sdk   # add to ~/.bashrc or ~/.zshrc
```

**Debian / Ubuntu:**

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib cmake ninja-build
git clone -b 2.3.1 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
export PICO_SDK_PATH=~/pico-sdk   # add to ~/.bashrc
```

Alternatively, install the official **Raspberry Pi Pico VS Code extension** — it manages the SDK, toolchain, ninja and picotool automatically (in `~/.pico-sdk`); the project's VS Code configuration in [`code/Beacon/.vscode/`](code/Beacon/.vscode/) is prepared for it.

### Building

```bash
cmake -B code/Beacon/build -S code/Beacon
cmake --build code/Beacon/build
```

The firmware binary is produced at `code/Beacon/build/Beacon.uf2`.

`picotool` (for USB flashing without the BOOTSEL button) is bundled with the VS Code extension, or can be built from [pico-sdk-tools](https://github.com/raspberrypi/picotool) / installed as `picotool` package on recent distros. Without it, use the BOOTSEL button method below.

### Flashing the built firmware

**Method 1 — BOOTSEL button:**

1. Hold the BOOT button and connect the board via USB.
2. The board appears as a USB mass-storage device (`RPI-RP2`).
3. Copy the firmware: `cp code/Beacon/build/Beacon.uf2 /run/media/$USER/RPI-RP2/`

**Method 2 — picotool (no button required):**

```bash
picotool load code/Beacon/build/Beacon.uf2 -fx
```

If the currently running firmware does not support USB reboot, use the BOOTSEL button method.

## GUI (configuration tool)

A PyQt5/PyQt6 desktop GUI is available for convenient configuration and monitoring: [`gui/beacon_gui.py`](gui/beacon_gui.py).

**Windows users:** a standalone build that needs no Python installation is available: [`gui/beacon_gui.exe`](gui/beacon_gui.exe) — just download and run. On first launch the SmartScreen may warn about an unsigned binary; choose "More info" → "Run anyway".

Install the Python dependencies:

```bash
pip install -r requirements.txt
```

This installs **pyserial** and **PyQt5**. If only **PyQt6** is available (or PyQt5 is not installed), the GUI automatically uses PyQt6 instead — pyserial is required in either case.

Run the GUI:

```bash
python3 gui/beacon_gui.py
```

Features:

- Serial port selection with auto-detection of the Pico CDC port, connect/disconnect
- Editing all parameters: message, CW tone, keying speed (WPM), tone sequence, TX mode, pause, PTT polarity
- **Transmission enabled** checkbox — temporarily stop the beacon (e.g. while tuning) and resume it later
- **Apply** — sends all fields to the device, **Save to flash** — `save`, **Read from device** — fills the fields from the device, **Defaults**
- Live device log, optional hiding of TX monitor output
- Manual command line for any console command

On connection the GUI automatically requests the current configuration from the device and fills in the fields.

![Beacon GUI screenshot](pics/rp2040_beacon_gui.png)

## Pin configuration

Pins and fixed parameters are defined at the top of [`code/Beacon/Beacon.c`](code/Beacon/Beacon.c):

```c
#define TONE_PIN          15   // Telegraph tone output
#define PTT_PIN           14   // PTT output
#define PTT_ACTIVE_LEVEL  1    // PTT active level
#define PTT_LEAD_MS       100  // PTT lead-in delay, ms
#define PTT_TAIL_MS       100  // PTT tail delay, ms
```

## License

MIT
