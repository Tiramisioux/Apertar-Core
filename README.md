# ApertarCore

ApertarCore is the camera engine behind [Apertar-UI](https://github.com/Apertar-Studio/Apertar-UI). It talks to the camera through libcamera, manages recording and still capture, and keeps the camera's settings in sync with the touchscreen interface, hardware buttons and WebUI.

You can use it with Apertar-UI for a complete camera setup, or control it from a browser without a touchscreen connected.

## What Can It Do?

- Record CinemaDNG, MLV, H.264 and ProRes, depending on the Raspberry Pi model.
- Take DNG, JPEG, PNG and TIFF stills, including paired RAW + processed images.
- Capture bursts, timelapses and three-shot exposure brackets.
- Use long exposures up to 30 seconds, where supported by the sensor and driver.
- Control ISO, shutter, frame rate, white balance, resolution and crop.
- Use Auto ISO, Auto Shutter and Auto White Balance, with exposure compensation and exposure metering.
- Control the camera over your local network with the built-in WebUI and live preview.
- Use physical buttons, rotary encoders and recording indicator LEDs.
- Record audio and supported motion-sensor data alongside your clips.
- Remember camera settings between restarts and work with Apertar's presets.

Available settings depend on your board, sensor, camera driver and tuning file. A selectable frame rate is not a promise that every recording format or storage device can sustain it.

## Recording Formats

| Format | What You Get |
| --- | --- |
| **CinemaDNG / cDNG** | A folder of RAW DNG frames, ready for a RAW editing workflow. |
| **MLV** | RAW video in an MLV file, with camera and white-balance metadata. |
| **H.264** | Compressed video in an MP4 file for smaller files and convenient playback. |
| **ProRes** | 10-bit 4:2:2 video in a MOV file, with Proxy, LT, 422 and HQ profiles. |

### CinemaDNG Compression

On Pi 5-class boards, the cDNG compression menu includes:

- **Uncompressed:** stores the selected RAW samples without compression.
- **Lossless JPEG:** reduces file size without changing the stored RAW samples.
- **Lossy 3:1:** trades some image information for smaller files.
- **Lossy 5:1:** applies stronger compression for smaller files again.

The lossy options use RAW companding/quantization followed by standard lossless JPEG encoding, with an embedded inverse curve. The ratio names are quality targets, not guaranteed file sizes; scene detail and noise affect compression.

RAW storage precision can be 8, 10 or 12 bits where the sensor and mode support it. Lossless compression preserves those selected stored samples; it does not recover information already lost earlier in the camera pipeline.

### ProRes Colour Profiles

ProRes supports the following colour-space/gamma choices:

- Rec709 / Gamma 2.4
- ARRI Wide Gamut 4 / LogC4
- Sony S-Gamut3.Cine / S-Log3
- DaVinci Wide Gamut / Intermediate
- P3-D60 / Linear

The Rec709 option keeps the selected tuning's rendering; its exact curve depends on that tuning. The other profiles use ISP approximations of the published transforms.

Assign the matching input colour space and gamma in your editor when needed.

CinemaDNG has a separate preview colour-profile setting. It changes monitoring, not the recorded RAW samples or their colour-space interpretation.

## Which Raspberry Pi Can I Use?

| Board Family | Available Clip Recording |
| --- | --- |
| **Pi 5 / CM5-class** | cDNG with uncompressed, lossless and lossy options; MLV; H.264; ProRes. |
| **Pi 4 / Pi 400 / CM4-class** | Uncompressed cDNG, MLV and H.264. |
| **Pi 3 / Pi 3A+ / Zero 2 W and other supported USB 2.0-only boards** | H.264. RAW clip formats and ProRes are disabled. |

**Still photography is separate from clip-format restrictions.** Selecting H.264 does not limit still photos to 1080p. Still resolutions and crops come from the available sensor modes.

On supported Pi 3/4-class boards, H.264 uses the hardware encoder, with output up to 1920x1080 at 30 fps. The sensor may impose a lower limit. Pi 5 uses software H.264 encoding instead.

While older Raspberry Pi boards have more limited processing power and memory, making some features unavailable, they're still excellent candidates for builds focused primarily on still photography.

## Cameras and Sensors

Core includes sensor-specific handling for:

- Sony IMX477
- Sony IMX577
- Sony IMX585
- Sony IMX294

Monochrome sensors are also handled when the driver and libcamera expose a supported monochrome RAW format. Mono capture does not use Bayer colour processing or white balance.

There is no single resolution/FPS list that applies to every setup. Available modes depend on the sensor driver, bit depth, board and recording format. Core reports compatible options to the UI and WebUI.

**Building Core does not install camera drivers, device-tree overlays, libcamera or tuning files.** These must already match your hardware and operating system.

## Still Photography

Still mode includes:

- Single capture and 3-, 5- or 10-shot bursts.
- A self-timer.
- DNG, JPEG, PNG, TIFF and supported RAW + processed pairs.
- Three-shot bracketing with an adjustable EV offset. Each frame keeps its actual exposure metadata; the images are not automatically merged into HDR.
- Long-exposure shutter choices up to 30 seconds, subject to sensor limits.
- Timelapse with selectable or custom intervals, including fractional seconds.

For long exposures, the preview remains responsive using a shorter exposure. During the actual shot, Apertar-UI shows a blackout and exposure countdown. Timelapse exposure choices are limited to fit the selected interval.

The UI reports **WRITING** while capture data is still pending and prevents another capture from starting before the previous work is complete.

## Audio and Motion Data

Clip audio is recorded as a separate Broadcast WAV file with timing metadata. Keep it alongside the picture files when copying a clip.

Audio settings support the sample rates, input channels and sample formats exposed by the connected device, including 16-, 24- and 32-bit PCM and 32-bit float where available. Selecting float does not make a microphone's ADC 32-bit or undo clipping.

Core aligns audio to sensor timestamps and handles capture discontinuities and small clock drift. H.264 and ProRes include a container timecode track; cDNG includes per-frame timecode. MLV stores capture-clock metadata, although an editor may not expose it as a native timecode track.

Supported IMUs can write a timestamped gyro sidecar for stabilization workflows. Supported drivers include MPU6050, BMI160, BMI270 and ICM42688-P. The sensor must be connected and configured correctly.

## WebUI

Open the camera's address from a phone, tablet or computer on the same network:

```text
http://<camera-ip>:8000
```

For example, an Apertar image configured with the hostname `apertar` and working mDNS can be reached at:

```text
http://apertar.local:8000
```

The WebUI provides live preview, camera controls, recording and still capture, settings, presets and media downloads. Core can run without the native touchscreen UI.

**Use a trusted LAN or camera hotspot.** The WebUI is not an authenticated internet-facing service. Same-origin checks are not a replacement for authentication; do not expose port 8000 directly to the internet.

Wi-Fi, hotspot, SSH and `.local` discovery require the corresponding system configuration. They are not installed by building Core alone.

## Tuning and Colour Calibration

Default tuning files are selected for both the sensor and the Raspberry Pi's image-processing pipeline:

| Platform | Default Folder | JSON Target |
| --- | --- | --- |
| Pi 5-class / PiSP | `/home/pi/Tuning-Files/defaults-pisp/` | `pisp` |
| Pi 3/4-class / VC4 | `/home/pi/Tuning-Files/defaults-vc4/` | `bcm2835` |

For example, an IMX585 on Pi 5 uses the compatible `imx585.json` from `defaults-pisp`. Pi 3/4 needs the VC4 version, not a renamed PiSP file.

Custom tuning files can be selected from `/home/pi/Tuning-Files/` or a `Tuning-Files` folder on recording media. Changing files restarts the camera pipeline, without rebooting the Pi. The last custom selection is remembered; clearing it returns to automatic sensor-default selection.

Manual white balance uses the selected tuning's calibration rather than hardcoded sensor gain tables. White balance is unavailable for mono sensors and is disabled for tuning files without the required calibrated AWB data.

User colour-profile overrides live in:

```text
/home/pi/apertar-color-profiles.conf
```

RAW metadata describes the sensor calibration and measured white balance. Preview LUTs, monitoring overlays and display calibration are not baked into RAW recordings.

## Hardware Controls

Hardware mappings are configured in:

```text
/home/pi/apertar-hardware.conf
```

Depending on the connected hardware, you can configure GPIO/PCF8574 buttons, rotary controls, recording LEDs, motion sensors and other supported accessories.

Buttons can step settings, jump to specific values, toggle monitoring tools, trigger standby or launch configured scripts. A button can run up to six actions, for example:

```ini
P0=iso200,fps24,wb5600
P1=focus_peaking,zebra
P2=capture
```

These are mapping examples, not a complete hardware configuration. Use the correct section, pin numbering and wiring for your device. Grouped toggles flip independently; they do not force every option into the same state.

The `run_script` action can run up to five configured scripts. Scripts run as the Core service user, without automatic administrator privileges. Only use scripts you trust.

## Boot Config (config.txt)

The WebUI's **System** pane has a **Boot Config** card for `/boot/firmware/config.txt`: the sensor on each camera connector, the IMX477/IMX585 link frequency, the RP1 overclock, the I²C, I²S, SPI and onboard-audio switches, and the display (HDMI only, Waveshare 4" or Hyperpixel 4" DPI).

The card edits the file the way its own comments say: it comments and uncomments the lines already there, in their `[pi5]`, `[pi4]` or `[all]` section, and adds a line next to its kind only when none exists. Every other line, the fan block included, stays byte for byte.

**Save & Reboot** first shows the lines that change. Core then reads the result back and refuses a save that would not do what you asked, for example when a line it does not edit still decides it, naming that line. The previous file is kept as `config.txt.apertar.bak`.

| Card | config.txt line |
| --- | --- |
| Camera 0 / Camera 1 sensor | `dtoverlay=<sensor>[,cam0][,mono][,link-frequency=<Hz>]` in the board's section; cam1 is the overlay default, without a port word |
| RP1 overclock | `dtoverlay=rp1-clk-333mhz` in `[all]` |
| I²C, I²S, SPI | `dtparam=i2c_arm=on`, `dtparam=i2s=on`, `dtparam=spi=on` in the board's section |
| Onboard audio | `dtparam=audio=on` in `[all]` |
| Display | the DPI overlays at the end of the file |

**Edit file** opens the whole file as text, for the lines the card has no control for. **Use This Text** reads it back onto the card. Save then applies the card's choices on top of your text and shows every line that changes, removed lines included, against the file on disk, with the same read-back, display confirmation and backup.

A display change asks you to confirm first: without its overlays a DPI panel stays dark after the reboot, and with them the panel holds GPIO 0-27, I²C bus 1 included.

`/boot/firmware` belongs to root. Install the small root helper the card and the fan mode write through:

```bash
sudo install -m 0755 -o root -g root ~/ApertarCore/scripts/apertar-boot-config /usr/local/libexec/apertar-boot-config
sudo install -m 0440 ~/ApertarCore/config/apertar-boot-config.sudoers /etc/sudoers.d/apertar-boot-config
sudo visudo -cf /etc/sudoers.d/apertar-boot-config
```

Without it, Core falls back to `sudo -n install`, as the fan mode always has.

If the camera does not come back after a change, put the card in a reader and copy `config.txt.apertar.bak` over `config.txt` on the boot partition.

Socket commands: `get_boot_config`, and `set_boot_config` with `cam0_sensor`, `cam1_sensor`, `cam0_link_frequency`, `cam1_link_frequency`, `i2c`, `i2s`, `spi`, `audio`, `rp1`, `display`, plus `dry_run`, `revision` and `confirm_display`, and optionally `text` (the whole file as edited; the other fields go on top of it).

## Building From Source

### Requirements

- A Linux environment with a matching camera driver and libcamera stack.
- CMake 3.20 or newer and a C++20 compiler.
- `pkg-config`, libyaml and ALSA development headers.
- libcamera and libcamera-base development files for real camera capture.
- FFmpeg/libav development files for the H.264 and ProRes plugins.
- JPEG development files for JPEG-dependent features; TurboJPEG is preferred when available.

On a compatible Raspberry Pi OS installation, the development packages typically include:

```bash
sudo apt install build-essential cmake pkg-config \
  libcamera-dev libyaml-dev libasound2-dev \
  libavcodec-dev libavformat-dev libavutil-dev \
  libjpeg-dev libturbojpeg0-dev
```

If your image uses a custom libcamera build, use its matching headers and libraries. Do not replace a working custom camera stack blindly with distribution packages.

### Compile

From the root of the Core repository:

```bash
cmake -S . -B ../ApertarCore-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../ApertarCore-build -j2
```

Use `-j1` on memory-limited boards if the compiler runs out of RAM.

The main outputs are:

```text
ApertarCore-build/
  apertar-core
  apertar-h264-libav.so
  apertar-prores-libav.so
```

The encoder plugins are built when the required libav development libraries are found. Keep them beside the executable when deploying a build, with their FFmpeg runtime dependencies available. Core loads them when needed.

Check CMake's output: it can configure without libcamera or libav, but that does not provide a working real-camera build or those recording backends.

The optional optimized Pi 5 ProRes codec has a separate build recipe under `src/prores/ffmpeg/`.

## Running Core

On an Apertar image that already has its service installed:

```bash
sudo systemctl start apertar-core.service
```

To run a source build manually, stop any existing Core service first so two processes do not compete for the camera:

```bash
sudo systemctl stop apertar-core.service
../ApertarCore-build/apertar-core \
  --socket /tmp/apertar-core.sock \
  --media /media/RAW
```

Run this from the Core repository folder. For a source-only setup without the service, skip the service command. Press `Ctrl+C` to stop the manual process; restart the service afterward if you normally use it.

Before recording, make sure the intended media is actually mounted at the selected path. An existing folder by itself does not prove that an external drive is mounted there.

### Launch Options

| Option | Purpose |
| --- | --- |
| `--socket PATH` | Local control/preview socket. Default: `/tmp/apertar-core.sock`. |
| `--media PATH` | Recording destination. Default: `/media/RAW`. |
| `--web-bind ADDR` | WebUI listening address. Default: `0.0.0.0`. |
| `--web-port PORT` | WebUI port. Default: `8000`; `0` disables it. |
| `--simulate` | Run without opening a physical camera, for interface development. |
| `--help` | Show command-line help. |

The service user needs access to the camera, recording media and configured hardware. Hardware H.264 also requires access to the encoder and DMA heap devices.

## Where Are Recordings Saved?

The default media root is `/media/RAW`. Clip folders contain the picture files and any associated audio, gyro or recovery sidecars. Still photos are saved in the `Photos` folder.

Names use the capture date and an incrementing counter. Keep a clip's associated files together when moving or backing it up.

Settings are saved separately from recordings. Core's camera settings normally live in `~/.config/apertar-core/camera_settings.conf`; shared UI settings and presets are managed by the Apertar application.

### Trim the recording drive

After about one card of writing without a trim, the NVMe stalls for seconds. Core trims the recording drive after clips are deleted (when a delete frees at least 1 GiB, also when the delete was made outside Core, for example from the touch UI), after the storage pre-roll's own clip is deleted, and when the drive is mounted, through a small root helper; `APERTAR_MEDIA_TRIM=0` turns it off.

```bash
sudo install -m 0755 -o root -g root ~/ApertarCore/scripts/apertar-media-trim /usr/local/libexec/apertar-media-trim
sudo install -m 0440 ~/ApertarCore/config/apertar-media-trim.sudoers /etc/sudoers.d/apertar-media-trim
sudo visudo -cf /etc/sudoers.d/apertar-media-trim
# proof: prints "... trimmed" and asks for no password
sudo -n /usr/local/libexec/apertar-media-trim /media/RAW
```

### Preview temporal denoise

The preview (HDMI, touch UI, WebUI, peaking, zebra) comes from the ISP, which blends every frame with the previous one (temporal denoise, `tdn` in the tuning file's `rpi.denoise`). **It is off by default**, because it is the most expensive step of the ISP back end. The recorded DNGs are the raw sensor frames and do not change either way; spatial denoise stays on.

At 4K Clear HDR 16-bit the ISP back end's job drops from about 37 ms to about 6 ms per frame, and 25 fps takes went from 39-116 lost frames per 3 minutes to 0-2 (measured 2026-11-04, probable).

Turn it on in the WebUI (Settings, System, Performance, "Preview temporal denoise") or with the command `set_preview_tdn enabled=true|false`; the choice is saved in `camera_settings.conf` (`preview_tdn=`) and the state reports `preview_tdn`. Changing it restarts the camera pipeline (no reboot) and is refused while recording.

Core does not edit your tuning files. With the switch off it resolves the tuning file as before (the automatic default and its Clear HDR variant, or the file you picked) and hands libcamera a copy without the `tdn` members, kept in `~/.cache/apertar-core/tuning/<name>_notdn.json` and renewed when the source file changes. If the file has no `tdn` member, or the copy fails its checks, the original is used and the journal says so (`ApertarCore preview tdn: ...`). `tuning_file` in the state stays your selection; `tuning_file_active` is the file libcamera loaded, and the journal line `ApertarCore tuning file:` ends in `preview_tdn=off active=<path>`.

`c++ -std=c++20 -I src tools/tuning_tdn_strip_test.cpp $(pkg-config --cflags --libs yaml-0.1)` builds the host test of the strip.

### Split clips (frames per folder)

On exFAT every new file walks all entries of its folder under the filesystem's lock, so a frame file costs more to create the fuller its clip folder is: about 14 ms at 45,000 frames (a 30-minute take at 25 fps), 35 % of the frame period (measured 2026-11-08, Apertar knowledge base `working/drop-frames/nvme-campaign/probe-exfat-dir.md`). `[storage] frames_per_folder=N` in `apertar-hardware.conf` (0, the default, keeps every frame in the clip folder; 100 to 100000) starts a new `part_NNN` folder inside the clip every N frames: frame f goes to `<clip>/part_<f / N>/` under its usual name, the numbering runs on across parts, and the WAV, `fps-ramp.txt` and the other sidecars stay in the clip folder, next to a `.parts` marker. Set it in the WebUI (Settings, System, Performance, "Frames per folder"), in the Config pane, or with `set_frames_per_folder value=N`; `APERTAR_CDNG_FRAMES_PER_FOLDER` wins over the file. It applies at the next take; the journal's `recording clip:` line ends in `frames_per_folder=N`. The storage pre-roll and MLV takes are never split. Core's clip list, playback, delete and download read both layouts.

**Resolve and Premiere import one folder as one clip**, so a split clip is flattened before import (move `part_*/*.dng` into the clip folder).

`c++ -std=c++17 -I src tools/frame_path_test.cpp` builds the host test.

### Write-path knobs for A/B tests

Most of these knobs can also be set in a media tuning file, so a test night needs no systemd drop-in: `~/apertar-media-tune.conf` (template: `config/apertar-media-tune.conf`; `cp` it, no sudo) and, above it, the override file `/run/apertar-media/media-tune.override.conf`, which lives in RAM and is gone at reboot (edit it with `scripts/apertar-tune`). Sections `[default]`, `[exfat]`, `[pi-nvme]`, `[pi-nvme.exfat]` and so on pick the medium; the most specific hit wins. Precedence per knob: environment variable, then override file, then `~/apertar-media-tune.conf`, then the built-in default. Core reads the files at start and when a take starts; the page-cache drop and the per-frame writeback are fixed for the life of the process, so restart Core to apply a change. Core logs the values in effect (`ApertarCore media tune: ...`) and a warning for every key it ignores.

Environment variables of the Core service (a systemd drop-in with `Environment=`) win over both files. Each one restores an older behaviour or switches a measured option on, so a change can be compared on the same rig. Details and the measurements behind them: the Apertar knowledge base, `working/dng-chain`. The last column is the knob's key in the file, where there is one.

| Variable | Default | What it changes | File key |
| --- | --- | --- | --- |
| `APERTAR_DNG_PAYLOAD_COPY=1` | off | Copies every frame's rows into the writer's scratch buffer before writing, even when the copy changes nothing (the behaviour before the direct write of 16-bit and 8-bit payloads). | - |
| `APERTAR_DNG_WRITE_ROWS=N` | 64 | Rows per `write()` when the payload goes through the scratch buffer (packed 10/12-bit, PiSP-compressed and CineMate Log frames). | - |
| `APERTAR_RAW_POOL_WARM=0` | on | Restores the old raw-copy pool: no copies allocated before a take starts, and at most workers + 2 kept while it runs. | - |
| `APERTAR_DNG_DONTNEED=writer` or `off` | sync | When a written DNG's pages leave the page cache: `sync` on the once-a-second sync thread after the file's `fsync`, when the pages are clean (cDNG only; lossy DNG then behaves as `off`); `writer` right after the write (the old default: the pages are still dirty then, so the call frees nothing and costs a cross-CPU LRU drain per frame, and the page cache fills until the kernel reclaims during the take); `off` never. | `dontneed` |
| `APERTAR_DNG_WRITEBACK=0` | on | No `sync_file_range` per written DNG (the kernel's own writeback only). | `writeback` |
| `APERTAR_MEDIA_SYNC_MS=0` | 1000 | No once-a-second `fsync` of the finished DNGs and the clip folder during a take; the stop-time `syncfs` stays. Also stops the default page-cache drop (`APERTAR_DNG_DONTNEED=sync` runs on this thread). | `sync_ms` |
| `APERTAR_RAW_CAMERA_BUFFERS=N` | 4 (8 in Clear HDR) plus the filesystem's extra | Number of raw camera buffers (3 to 16), each with its own libcamera request. | `raw_buffers` (absolute count); `extra_raw_buffers` (0 to 8) adds to the built-in count instead |
| `APERTAR_WEB_PREVIEW_RECORDING_FPS=N` | 30 | The WebUI live preview's frame rate while a raw take runs (`0` pauses it). Its downsample runs on libcamera's completion thread, inside the per-frame cycle, whenever a browser shows the preview (about 0.2 to 0.5 ms per frame). | - |
| `APERTAR_RT_CAPTURE_PRIORITY=N` | off | `SCHED_FIFO` priority N (1 to 99) for libcamera's completion thread alone, set from Core when the first frame arrives; the writers and the WebUI keep their normal priorities. Needs `LimitRTPRIO` on the unit (the capture-priority drop-in). The preferred shape of the scheduling A/B. | - |
| `APERTAR_RT=1` | off | The start script runs the whole Core process under `SCHED_FIFO` priority 70 on cores 1 to 3 with the best-effort I/O class (`APERTAR_RT_PRIORITY`, `APERTAR_RT_CPUS` adjust it), as cinemate runs cinepi-raw. An A/B instrument, not a recommendation: equal-priority FIFO threads never preempt each other, so the capture thread can wait behind FIFO writers. Needs `LimitRTPRIO` on the unit: install `config/apertar-core-capture-priority.conf` as a drop-in (`sudo install -m 0644 config/apertar-core-capture-priority.conf /etc/systemd/system/apertar-core.service.d/` and `daemon-reload`). | - |
| `APERTAR_CDNG_FRAMES_PER_FOLDER=N` | 0 | Frames per `part_NNN` folder inside the clip (100 to 100000; 0 = one folder); see "Split clips". Wins over the hardware file's `[storage] frames_per_folder`. | - |
| `APERTAR_MEDIA_TUNE=1` | off | Kernel-side knobs for the recording drive through the `apertar-media-tune` root helper (dirty-page cushion, scheduler, queue depth). | `kernel_tune` (`on` / `off`) |

## For Developers

ApertarCore is the source of truth for camera and capture state. Native UI commands, WebUI requests and hardware actions reach the same controller and busy-state checks.

```text
Apertar-UI / WebUI / Hardware Controls
                  |
                  v
             ApertarCore
                  |
               libcamera
                  |
       RAW / processed camera streams
                  |
        Recording, stills and preview
```

The native UI uses a Unix socket with line-delimited JSON commands and asynchronous events. Preview buffers are shared using DMA-BUF file descriptors rather than copying every frame through JSON. Browser preview uses MJPEG instead.

A few socket examples, with `socat` installed:

```bash
printf '{"cmd":"get_state"}\n' | socat - UNIX-CONNECT:/tmp/apertar-core.sock
printf '{"cmd":"set_fps","value":24.0}\n' | socat - UNIX-CONNECT:/tmp/apertar-core.sock
printf '{"cmd":"record_start"}\n' | socat - UNIX-CONNECT:/tmp/apertar-core.sock
printf '{"cmd":"record_stop"}\n' | socat - UNIX-CONNECT:/tmp/apertar-core.sock
```

The WebUI also exposes current state at `GET /api/state`.

Background recording workers and bounded queues help keep camera requests moving. Recording errors and detected dropped frames are reported rather than hidden. Capture and media-operation guards prevent conflicting actions while data is still being written, but cannot prevent every kernel, driver, storage or power failure.

## Recording Safely

- Use storage fast enough for your selected format, resolution and frame rate.
- Wait for recording to stop and **WRITING** to finish before ejecting media.
- Use the camera's normal shutdown before disconnecting power.
- Update Core and Apertar-UI together so their controls and capabilities match.

## Credits

ApertarCore builds on libcamera, Raspberry Pi's camera pipeline and other open-source projects. Inspiration comes from [Schoolpost's cinepi-raw](https://github.com/cinepi/cinepi-raw).
Thanks to the developers, contributors and camera builders who help test and improve Apertar.
