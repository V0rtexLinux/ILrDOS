# ILrDOS — Intelligent Lifeform robot Disk Operating System

C++ runtime for the robot (Raspberry Pi OS or the custom image in `os/`). Since 2.0.0 the robot speaks, listens, moves, controls IoT devices, holds conversations, enrolls and recognizes faces — all decided by the robot itself. The target architecture is the Raspberry Pi Zero 2 W, with GPIO 18 (eye) and GPIO 23 (red alert) as described in the original material.

**Contents**

1. [Overview and Features](#1-overview-and-features)
2. [Quick Start](#2-quick-start)
3. [Using ILrDOS as a C++20 Library](#3-using-ilrdos-as-a-c20-library)
4. [Fleet Automation Example and Remote Terminal](#4-fleet-automation-example-and-remote-terminal)
5. [Security Notes](#5-security-notes)
6. [Input Files and Models](#6-input-files-and-models)

---

## 1. Overview and Features

| Capability | How it works |
|---|---|
| **Offline voice commands** | Always listening (Vosk PT-BR, running on the Pi itself). Say *"ILrDOS, …"*; afterwards it accepts replies for 10 s without repeating its name. Known commands are understood locally (works without a PC); everything else goes to Ollama on the PC. Without a PC it still obeys commands and introduces itself. |
| **Speech** | TTS from the PC (GLaDOS/Kokoro, port 5050) with local `espeak` as fallback. |
| **Movement** | H-bridge + HC-SR04 sonar. It faces whoever is speaking or standing in front of it (`track`), approaches while stopping at a distance, and wanders on its own (`roam`). |
| **IoT** | `ilrdos-iot.txt`: local HTTP (Tasmota, Shelly, Home Assistant…), MQTT and GPIO relays, plus rules (*someone arrives → turn on the light*). |
| **Privacy-focused vision** | YuNet detects faces; SFace (OpenCV) recognizes them. Only numeric vectors are stored — never photos. Greets people by name and triggers per-person rules. |
| **Multi-robot coordination** | Commands other robots (vacuums, mops, lawn mowers, pool robots, AGVs, conveyors, arms, forklifts) over MQTT, Modbus, HTTP and BLE. |
| **Embedded self-perception** | The robot senses each part of its body in the first person and uses that in conversation. It is honest that it has a *model of itself*, not proof of consciousness. |
| **Autonomy** | Greets, tracks, roams, adjusts vision according to heat, restarts Wi-Fi, and speaks up when something fails. |

### Robot structure (2.1.0)

The OS follows the project's design. Each part is an entry in `include/ilrdos/structure.hpp` with a state (`ok`, `atencao`, `falha` or `sem-sensor`):

```
[main power cables  - / +] --> [brain / main computer]
[visor / camera] == data transmission cables ==> [brain / main computer]
[protective casing] surrounds the brain
```

| Part | What the OS checks |
|---|---|
| **Main power cables (− and +)** | Low-voltage warning from the Pi firmware (`get_throttled` or hwmon `rpi_volt`). Without that sensor: `sem-sensor`. Alternate path: `THROTTLED_FILE`. |
| **Brain / main computer** | CPU temperature, load and uptime. Hot = attention; critical = failure. |
| **Data cables (camera/visor → computer)** | `/dev/video<N>` exists and the camera delivers an image. |
| **Protective casing** | Impact sensor (fall/hit): stops the motors and emits 3 soft beeps (~900 Hz, 30% duty, not deafening). State is `atencao` for 30 s after a fall. Also monitors internal temperature (wheels stop and vision is reduced when it heats up). |
| **Visor / camera** | Camera open, faces in the last scan, whether recognition is active. |

Terminal: `structure` (all parts), `structure map` (the drawing as text) and `structure power|brain|datalink|casing|visor` (one part). `status` shows `estrutura=...`, and the spoken question *"how are you?"* mentions only what is not OK.

### Movement safety

- Every movement has a deadline (max 3 s) and stops by itself if the daemon or network goes down.
- Speed is limited (`MOTOR_MAX_SPEED`, default 60%). Reverse is shorter and slower (no rear sensor).
- Moving forward, the sonar stops the robot below `SONAR_STOP_CM`, and **no sonar reading = it does not move**. Autonomous roaming only exists with sonar.
- **Movement respects the drawn structure:** accumulated rotation is counted and limited to `MAX_TWIST_DEG` (default 270°) so the power cables (−/+) and the visor data cable don't get stretched. At the limit it only turns back the other way. `untwist` undoes the twist; `untwist zero` resets it after you check the cables by hand. Twist is estimated by time (no encoder) and saved in `TWIST_FILE`.
- Smooth start and stop on a smoothstep curve, like GLaDOS's chassis hanging from cables (`MOTOR_RAMP_MS`). Direction reversal only after a pause (`REVERSE_SETTLE_MS`).
- The visor sits at the end of a long body and looks only forward, so turns use a ramp 50% longer and reverse is slow and short (30%, 1 s).
- After an impact on the casing it does not move for `IMPACT_LOCKOUT_MS`. **A fall detected by the casing sensor stops the motors immediately** and emits a soft audible alert (3 × 220 ms at ~900 Hz, 5 s between alerts).
- Low voltage on the power cables or critical heat in the main computer = it does not move.
- `stop` always wins over everything.
- Speech is treated as data: the language model can only trigger actions from the allowed list, under the same limits.
- **Test first with the wheels off the ground** and keep a physical battery switch within reach.
- The HC-SR04 uses 5 V: connect ECHO to the Pi through a voltage divider (e.g. 1 kΩ + 2 kΩ) to avoid burning the GPIO.

### Faces and privacy (biometric data)

- Enrollment requires consent: via terminal (`face enroll Maria`) or by voice, when it asks the name, repeats that it will store it, and requires you to say **"eu confirmo"** ("I confirm"). It only enrolls with **one** person in front of it. It never enrolls strangers on its own.
- Data stays on the SD card (`ilrdos-faces.db` on BOOT; `/var/lib/ilrdos/faces.db` on Pi OS), permission 0600. Deleting the file deletes everything. `face forget <name|all>` via terminal; by voice only *"forget my face"* (and only if it is recognizing you).
- Recognition **is not authentication**: `sensitive=1` devices (doors, gates…) only obey the authenticated terminal — never voice, AI, rules or face (a photo could fool the camera).
- At fairs/schools, notify people and enroll only those who agree (LGPD treats biometrics as sensitive data; minors need guardian authorization).
- `SEND_NAMES_TO_LLM=0` prevents names from reaching Ollama.

### Phrases understood without the PC

*"start the vacuum"*, *"vacuum, go back to the dock"*, *"stop everything"*, *"move forward two seconds"*, *"turn left ninety degrees"*, *"stop"*, *"turn on the living room light"*, *"turn off the fan"*, *"who am I?"*, *"who is here?"*, *"who do you know?"*, *"save my face"*, *"forget my face"*, *"how are you?"*

(The built-in phrases are in Brazilian Portuguese.)

### Home and industry

Besides Wi-Fi (HTTP/MQTT/Home Assistant) and GPIO relays, it controls wired Modbus equipment (TCP and RS-485/RTU, with state read back and a minimum interval between commands), reads wired sensors (door/window contacts and presence on GPIO, 1-Wire temperature, Modbus registers: `sensor ...`, `iot read`, *"what is the boiler temperature?"*) and switches declared Bluetooth LE actuators. Machines with `confirm=1` only start by voice with "eu confirmo" and never by rule or by the model. Phones, routers and computers are always refused.

**ILrDOS does not replace emergency stops or safety interlocks.**

### Version history (1.x)

- Linux service that starts with the Raspberry Pi.
- LED patterns `idle`, `listen`, `think`, `speak`, `alert` and `off`.
- Windows remote console over the local network, with HMAC-SHA256 challenge authentication.
- Allowed commands: `help`, `status`, `eye <state>`, `vision status`, `vision scan`, `autonomy on|off|status` and `exit`.
- Autonomy (v1.1.0): the daemon reacts to faces and looks after itself (heat, Wi-Fi, camera); see `os/README.md`.
- YuNet face detector in C++/OpenCV; no image is saved (since 2.0.0 it also identifies faces enrolled with consent).
- Optional Python voice bridge: Vosk + Silero VAD on the Pi, Ollama on the PC, and remote GLaDOS TTS with `espeak` as fallback.
- The assistant uses short in-memory context and answers with an educational focus; transcribed text goes only to the Ollama instance configured on the local network.
- No shell command is executed over the network.
- `--simulate` mode to demo without the hardware connected.

The daemon and robot control remain in C++; the voice bridge uses Python because it reuses the audio/Vosk libraries from the Pi package. It does not control GPIO directly: it only sends allowed commands to the authenticated daemon.

---

## 2. Quick Start

### Simulation mode on Linux (`--simulate`)

Requires Linux with `g++`, Linux headers and OpenSSL development files:

```sh
cd robot/ilrdos
./scripts/build-linux.sh
umask 077
openssl rand -hex 32 > /tmp/ilrdos-token
./build/ilrdosd --simulate --bind 127.0.0.1 --port 4317 --token-file /tmp/ilrdos-token
```

In another terminal, run `./build/ilrdos-client 127.0.0.1 4317`, type the token, and try `status`, `eye alert` and `exit`.

To exercise the whole flow without hardware, `tests/sim-test.sh` starts the daemon in `--simulate` and tests conversation, movement, sonar, IoT, rules, face enrollment/recognition and tracking.

### Native install on the Raspberry Pi Zero 2 W

Use Raspberry Pi OS 64-bit and a private Wi-Fi network. Copy `robot/ilrdos` to the Pi and run:

```sh
sudo apt update
sudo apt install -y build-essential libssl-dev pkg-config libopencv-dev
cd robot/ilrdos
./scripts/install-pi.sh
hostname -I
```

The installer builds the service on the Pi itself, installs the YuNet model and a `systemd` unit. OpenCV 4 is required for detection; without it the daemon keeps working but reports that vision is not compiled. Connect a V4L2-compatible USB camera at `/dev/video0`; the code never records the image.

**Wiring** (per the original `FIACAO.md`): GPIO BCM 18 to the eye LED and BCM 23 to the red LED, always with a 220 Ω series resistor; common GND. Never connect an LED without a resistor.

**Configuration** lives on the BOOT partition (or `/etc/ilrdos/` on Pi OS). Commented examples are in `os/board/ilrdos/`:

- `ilrdos-robot.txt` — pins, PC, limits
- `ilrdos-iot.txt` — devices and rules
- `ilrdos-fleet.txt` — other robots

To read the token on the Pi without changing it: `sudo cat /etc/ilrdos/token`. Do not show it on a projector or send it by chat.

### Voice and TTS setup

> **Note (2.0.0):** conversation is now native in the C++ daemon. The Python `voice/` bridge below is the legacy path and must not run at the same time (both would use the microphone).

The conversation model is not inside the ZIPs, and the Pi Zero 2 W is not set up to run a large LLM. Voice uses Ollama on the fair's computer. To install the bridge on the Pi:

```sh
./scripts/install-voice-pi.sh
sudo nano /etc/ilrdos/voice.env
```

Replace `ILRDOS_PC_IP` with the PC's IP. On the PC, start Ollama, pull the model named in `ILRDOS_OLLAMA_MODEL` (`qwen2.5:7b-instruct` by default) and allow access only from the private Wi-Fi network. Then:

```sh
sudo systemctl enable --now ilrdos-voice
sudo journalctl -u ilrdos-voice -f
```

The voice installer downloads the small Vosk PT-BR model (~40 MB) from the official distribution, since it is not attached, and installs the supplied Silero VAD ONNX. The remote GLaDOS TTS on port 5050 is optional; without it the Pi tries `espeak`. The attached phonemization/GLaDOS models are English; they do not change the personality of the answers nor provide adequate PT-BR speech.

**Original GLaDOS voice on the PC:** extract `glados-feira.zip`, put `glados.onnx` and `phomenizer_en.onnx` in `models/TTS/` together with the ZIP's JSON/PKL files, and run:

```bat
robot\ilrdos\scripts\start-glados-tts-windows.bat C:\path\to\glados-feira
```

The service uses the GLaDOS voice weights and answers the endpoint the Pi bridge already calls. It is only the voice: the ILrDOS personality comes from the Ollama prompt. This TTS was trained for English, so Portuguese pronunciation may be limited. On Windows, allow port 5050 only on the private local network.

**Kokoro on the PC:** put `kokoro-v1.0.fp16.onnx`, `kokoro-voices-v1.0.bin` and `phomenizer_en.onnx` in `models/TTS/`, then run:

```bat
robot\ilrdos\scripts\start-kokoro-tts-windows.bat C:\path\to\glados-feira
```

On the Pi, set `ILRDOS_TTS_VOICE=af_alloy` in `/etc/ilrdos/voice.env` and restart `ilrdos-voice.service`. To return to the GLaDOS voice, use `ILRDOS_TTS_VOICE=glados` with the GLaDOS launcher. Kokoro also uses English phonemization and is not optimized for the Pi Zero 2 W.

---

## 3. Using ILrDOS as a C++20 Library

ILrDOS is an open-source (MIT), **header-only** C++20 library (`include/ilrdos/`), and `ilrdosd` is just a program that uses it. Any project can use the robot structure, the fleet, IoT, etc. as an API.

### CMake `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(ilrdos GIT_REPOSITORY https://github.com/V0rtexLinux/ILrDOS.git GIT_TAG v2.2.0)
FetchContent_MakeAvailable(ilrdos)

add_executable(my_robot main.cpp)
target_link_libraries(my_robot PRIVATE ilrdos::ilrdos)
```

```cpp
#include <ilrdos/ilrdos.hpp>
```

### Installed package

Alternatively, install it (`cmake --install build-cmake`) and use:

```cmake
find_package(ilrdos CONFIG REQUIRED)
```

### CMake options

`ILRDOS_WITH_OPENCV`, `ILRDOS_WITH_ALSA`, `ILRDOS_WITH_VOSK`, `ILRDOS_BUILD_DAEMON`. The only mandatory dependency is OpenSSL. See [CONTRIBUTING.md](CONTRIBUTING.md) to modify and contribute.

---

## 4. Fleet Automation Example and Remote Terminal

### Fleet: ILrDOS coordinating other robots (2.2.0)

ILrDOS commands **other robots**: vacuums, mops, lawn mowers, pool robots and industrial machines (AGVs, conveyors, arms, forklifts). It does not pilot each robot; it sends high-level commands over the channel the robot already has and coordinates who works where and when. The actions are the same for every brand: **start**, **stop**, **pause** and **dock** (return to base/charge).

| Channel | Used for |
|---|---|
| `http` | Home Assistant, Roborock/Xiaomi via integration, any local API |
| `mqtt` | Valetudo (START/STOP/PAUSE/HOME patterns), Tasmota, ESPHome |
| `modbus` / `rtu` | PLCs, AGVs, conveyors, industrial robots (TCP or RS-485 by cable) |
| `ble` | Toy robots and small Bluetooth LE robots |

Configuration goes in `ilrdos-fleet.txt` (BOOT, or `/etc/ilrdos/fleet.txt`; commented example in `os/board/ilrdos/ilrdos-fleet.txt.example`):

```
robot vacuum  mqtt host=192.168.1.10 topic=valetudo/vacuum/BasicControlCapability/operation/set class=vacuum zone=home alias=vacuum
robot mop     http start=http://192.168.1.20/start stop=http://192.168.1.20/stop dock=http://192.168.1.20/dock class=mop zone=home alias=mop
robot agv1    modbus host=192.168.1.40 unit=1 coil_start=0 coil_stop=1 class=agv zone=aisle_a alias=agv
group cleaning vacuum,mop alias=cleaning,house
rule empty -> vacuum start          # nobody in sight: start cleaning
rule arrive any -> vacuum dock      # someone arrived: vacuum returns to its base
rule arrive unknown -> agv1 stop    # unknown person in the area: stop the AGV
```

**What coordination does**

- **Voice, no PC:** *"start the vacuum"*, *"tell the mop to clean"*, *"vacuum, go back to base"*, *"how is the vacuum doing?"*, *"clean the house"* (group), *"stop everything"* / *"emergency"* (stops the whole fleet at once).
- **Presence rules:** the robot sees and hears the house and decides: everyone leaves, the vacuum starts; someone arrives, it returns to base.
- **Zones (`zone=`):** two robots in the same zone don't work together (vacuum and mop in the same room, two AGVs in the same aisle). The second waits for the first to stop or to pass `max_run` minutes.
- **Groups:** one command for several robots; group `stop` and `stop everything` run in parallel, so an offline robot doesn't delay the others.

**Fleet safety**

- **`stop`, `pause` and `dock` always apply**, from any origin (voice, rule, AI, terminal), with no confirmation or minimum interval: they are the safe direction. Every robot must declare `start` and `stop`.
- **`start` of heavy machinery** (industrial `class=`, agv, amr, arm, conveyor, forklift, cnc, machine…), of **lawn mowers** (blade) or of any robot with `confirm=1` only happens by voice with **"eu confirmo"** (one robot at a time) or through the authenticated terminal. Never by automatic rule, by the AI model's say-so, or by group.
- **`sensitive=1`:** only the authenticated terminal can start the robot.
- Phones, routers and computers are never accepted; addresses must be local network only; only robots declared in the file exist (no URLs coming from speech or the network).
- The displayed state is that of the last command, or what the robot itself reports (`status=` / `status_reg=`); text coming from the robot is treated as data.
- **ILrDOS does not replace emergency stops, light curtains or interlocks.** Fleet `stop` is a network command and can fail if the robot is offline; in industry, safety remains in hardware.

### Terminal commands

| Command | Purpose |
|---|---|
| `fleet list` / `fleet status [name]` | List robots / show state |
| `fleet stop all` | Stop the entire fleet |
| `fleet <robot\|group> start\|stop\|pause\|dock` | Command a robot or group |
| `face list\|who\|enroll <name>\|forget <name\|all>\|cancel` | Face management |
| `move forward\|back\|left\|right [ms] [speed]` | Timed movement |
| `turn left\|right [degrees]` | Rotate |
| `untwist [zero]` | Undo cable twist (`zero` resets the counter) |
| `stop` | Stop motion |
| `iot list\|<device> on\|off\|toggle` | IoT devices |
| `say <text>` | Speak text |
| `hear <text>` | Simulate heard speech |
| `voice status` | Voice state |
| `structure [map\|power\|brain\|datalink\|casing\|visor]` | Body parts |
| `impact` / `impact test` | Impact sensor state / trigger alert manually |
| `autonomy motion off\|track\|roam` | Motion autonomy mode |

### Remote terminal: Linux

Connect with the C++ client (built by `scripts/build-linux.sh`):

```sh
./build/ilrdos-client 192.168.1.50 4317
```

Replace `192.168.1.50` with the address shown by `hostname -I` on the Raspberry Pi. The client asks for the token without displaying it.

### Remote terminal: Windows

Open the **Developer Command Prompt for Visual Studio** and run:

```bat
cd robot\ilrdos
scripts\build-windows.bat
build\ILrDOS-Terminal.exe 192.168.1.50 4317
```

Replace `192.168.1.50` with the address shown by `hostname -I` on the Raspberry Pi. The client asks for the token without displaying it. MSVC/Visual Studio Build Tools with the Windows SDK are required to build the `.exe`.

---

## 5. Security Notes

- Keep the Pi and the computer on the same private network under your control, preferably a dedicated hotspot with a WPA2/WPA3 password.
- Do not port-forward port 4317 and do not use public Wi-Fi.
- The token is validated by HMAC challenge and is never sent over the network.
- The protocol does **not** encrypt session content. Use it only on a trusted local network.
- The service refuses a token file readable by group/others and rate-limits authentication attempts per address.
- Vosk transcribes audio on the Pi; text questions and face-detection context go to the Ollama/TTS configured on the PC. No frame is sent or saved.
- No shell command is ever executed over the network.

---

## 6. Input Files and Models

- **Included/integrated:** YuNet (faces) and Silero VAD (voice activity).
- **Parakeet v2** now has an encoder and decoder, but the matching v2 joiner is still missing; the v3 weights provided do not form a complete set either. Do not mix the v3 joiner with v2.
- `vision_encoder_fp16` is only a visual encoder, without the language model needed to describe images.
- **GLaDOS and Kokoro** can synthesize speech on the PC but use English phonemization.
- The **ILrDOS personality** is defined by the Ollama prompt.
- **PT-BR recognition** keeps using Vosk, downloaded separately.
