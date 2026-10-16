# Beacon — CW (Morse) Beacon for HF or VHF Radio

A Morse-code beacon firmware for the [Waveshare RP2040-Zero](https://www.waveshare.com/wiki/RP2040-Zero) board (Raspberry Pi RP2040), designed for keying a VHF FM/SSB transmitter.

The firmware generates an audio telegraph tone on one GPIO, controls transmitter PTT on another, transmits a configurable message stored in non-volatile (flash) memory, and repeats it with a configurable pause. All beacon parameters (tone frequency, message text, pause, keying speed, transmission mode) are configurable at runtime over a USB serial console and persist across power cycles.

## Features

- **Telegraph tone output** — clean sine wave (DDS over a 62.5 kHz PWM carrier) with 5 ms raised-cosine rise/fall to avoid key clicks, 100–5000 Hz (600/800 Hz typical for FM audio), default **800 Hz**
- **PTT control** — active-high output, asserted at the start of the transmission and released after the end of the message, with configurable lead/tail delays
- **Morse message** — full A–Z, 0–9, `. , ? / - = @ : + ( ) " ' !` and prosigns (`<SK>`, `<BT>`, `<AR>`, …) encoding with standard element timing; speed set directly in WPM (default **20 WPM**)
- **Tone sequences** — an arbitrary chain of tone bursts ("freq:ms" pairs, `0` = silence), transmittable instead of (or after) the Morse message
- **Transmission modes** — `cw` (Morse only), `tones` (tone sequence only), or `both` (Morse, then tones) within one PTT
- **Non-volatile configuration** — stored in the last sector of the onboard flash; survives power-off and reflashing
- **Ready-to-flash firmware** — a prebuilt `.uf2` binary is included in [`firmware/`](firmware/), no toolchain required
- **USB console** — live configuration over the USB CDC serial port
- **Transmitter safety** — time-out timer (PTT is forcibly released after 120 s), hardware watchdog (reboot with PTT off if the firmware hangs), `txen 0` aborts the transmission immediately
- **Accurate keying** — the whole transmission is laid out in advance and played by a hardware timer, so console activity and USB output do not affect the timing
- **Status LED** — onboard WS2812 (GP16): steady green in the pause, red blinking in sync with the keying during transmission; brightness configurable

## Hardware

| Signal    | GPIO | Description                                      |
|-----------|------|--------------------------------------------------|
| Tone      | GP15 | Telegraph tone output → transmitter audio input  |
| PTT       | GP14 | PTT output → transmitter PTT line                |
| CW key    | GP7  | Key output → transmitter key input via transistor (keyed while the tone sounds) |
| WS2812    | GP16 | Onboard status LED (built into the board)        |

> Connect the tone output to the transmitter's audio/modulation input (use an attenuator or level shifter if required by your radio) and the PTT output to the transmitter's PTT input, sharing a common ground. The tone output idles at mid-supply (~1.65 V DC) and carries a sine of ~3.1 V p-p, so the audio input must be AC-coupled (a series capacitor of 1–10 µF if the radio has none). The 62.5 kHz PWM carrier is far above the audio band and is removed by the transmitter's audio filtering; for a cleaner signal add an RC low-pass (e.g. 1 kΩ + 22 nF, ≈7 kHz) at the pin. The CW key output drives the transmitter's built-in keyer input through an external transistor (e.g. 2N2222 / IRLML6402 depending on key input polarity): GPIO → 1k resistor → transistor base, key line on the collector/emitter side. The key is closed while the tone sounds (dots/dashes in CW mode, active entries of the tone sequence).

![Waveshare RP2040-Zero pinout](pics/rp2040_zero_pinout.png)

## Flashing

A prebuilt firmware binary is included in the repository: [`firmware/Beacon-v0.2.uf2`](firmware/Beacon-v0.2.uf2). You do not need to build anything to flash it.

**Method 1 — BOOTSEL button:**

1. Hold the BOOT button and connect the board via USB.
2. The board appears as a USB mass-storage device (`RPI-RP2`).
3. Copy the firmware: `cp firmware/Beacon-v0.2.uf2 /run/media/$USER/RPI-RP2/`

**Method 2 — picotool (no button required):**

```bash
picotool load firmware/Beacon-v0.2.uf2 -fx
```

If the currently running firmware does not support USB reboot, use the BOOTSEL button method.

## Configuration commands (USB console)

Connect to the board's serial port (`/dev/ttyACM0` on Linux, e.g. `minicom -D /dev/ttyACM0` or `picocom /dev/ttyACM0`). Any baud rate works.

Type the command and press **Enter**. If your terminal can send no line ending, the line is executed automatically after a 5-second pause in typing.

| Command        | Description                                        |
|----------------|----------------------------------------------------|
| `msg <text>`   | Set message text (A–Z, 0–9, `. , ? / - = @ : + ( ) " ' !`); prosigns in angle brackets are sent without letter gaps, e.g. `<SK>`, `<BT>`. Max 95 characters |
| `tone <hz>`    | Set tone frequency, 100–5000 Hz                    |
| `pause <sec>`  | Pause between message repeats, 0–3600 s            |
| `wpm <5..60>`  | Keying speed, words per minute (dot = 1200 / wpm)  |
| `tones <f:d> ...` | Tone sequence: `freq:ms` pairs, freq 0–5000 Hz (`0` = silence), duration 10–10000 ms, up to 24 entries. Without arguments — show current sequence |
| `txmode cw\|tones\|both` | What to transmit: Morse only / tone sequence only / both |
| `txen <0\|1>`   | Enable/disable transmission (0 = stops immediately, PTT released; beacon stays silent, e.g. while tuning) |
| `led <0..100>` | Status LED brightness, % (default 20)              |
| `save`         | Write current configuration to flash (during a transmission it is deferred until the transmission ends) |
| `show`         | Print current configuration                        |
| `default`      | Restore factory defaults                           |
| `ver`          | Firmware version (program version and git revision) |
| `help`         | Show command list                                  |

Changes take effect immediately (from the next transmission); use `save` to make them permanent. Invalid values are rejected with an `error:`/`usage:` reply and leave the setting unchanged. When the message, speed, mode or tone sequence makes a transmission longer than the 120 s time-out, a `warning:` is printed — such a transmission would be cut off. `show` also prints the transmission length (`txlen`).

The configuration is stored with a version number and CRC-32; a configuration saved by firmware v0.1 is migrated automatically on first boot.

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

## Helper scripts

| Script | Purpose |
|--------|---------|
| [`scripts/build.sh`](scripts/build.sh) | Configure (once) and build the firmware → `code/Beacon/build/Beacon.uf2` |
| [`scripts/flash.sh`](scripts/flash.sh) | Flash the built firmware to the board via picotool (auto-detects the picotool from the Pico VS Code extension; override with `PICOTOOL=/path/to/picotool`) |
| [`scripts/build_gui_exe.bat`](scripts/build_gui_exe.bat) | (Windows) Build the standalone `gui/beacon_gui.exe` with PyInstaller |
| [`scripts/update_firmware.sh`](scripts/update_firmware.sh) | Copy the freshly built binary to the tracked [`firmware/Beacon-v0.2.uf2`](firmware/Beacon-v0.2.uf2) |

Typical workflow: `./scripts/build.sh && ./scripts/flash.sh`, then `./scripts/update_firmware.sh` before committing a new firmware version.

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
- Editing all parameters: message, CW tone, keying speed (WPM), tone sequence, TX mode, pause, LED brightness; the message field only accepts characters the firmware can send
- **Apply** sends the fields one by one, waiting for the device's reply to each; rejected values and time-out warnings are reported in a summary dialog
- Firmware version display; a lost connection (cable unplugged, board reset) is detected automatically
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
#define PTT_PIN           14   // PTT output (active high)
#define KEY_PIN           7    // CW key output (active high)
#define PTT_LEAD_MS       1000 // PTT lead-in delay, ms
#define PTT_TAIL_MS       100  // PTT tail delay, ms
#define TX_MAX_MS         120000 // Time-out timer: max PTT time per transmission, ms
#define WATCHDOG_MS       3000 // Watchdog timeout, ms
```

The PTT and CW key outputs are always **active high** (logic 1 while transmitting / keyed) — this is fixed in the firmware, no configuration needed.

## License

MIT
