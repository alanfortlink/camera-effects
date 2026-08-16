# Iris — system-wide camera effects for Omarchy

> Tested only on **Omarchy 4** (Arch Linux, Hyprland, omarchy-shell). Video walkthrough: coming soon.

![Iris panel](docs/panel.png)

macOS-style camera effects for Omarchy: a background daemon reads your webcam,
applies effects, and publishes the result as a virtual webcam called
**"Iris Camera"** that every app (Chromium, Zoom, Discord, OBS, Firefox…)
sees like a normal camera. A Quickshell plugin adds a camera icon to the bar
that lights up while an app uses the camera and opens a Control-Center-style
panel with a live preview and the toggles.

Effects: **Center Stage** (auto-framing), **Portrait** (background blur with
intensity), **Studio Light** (face lift, dimmed room), **Background**
(color/image), **Reactions** (hearts, thumbs up/down, balloons, confetti,
fireworks, rain, lasers — by button or by hand gesture, same gesture set as
macOS), **Mirror**. Everything runs on the CPU (small ONNX models); ~20 ms per
720p frame on a desktop.

```
repo/
  daemon/    irisd — C++ daemon (V4L2 capture → effects → v4l2loopback + native PipeWire node)
  plugin/    Quickshell plugin for omarchy-shell (Service.qml, Panel.qml, Preview.qml)
  scripts/   iris-setup — the privileged part (device at boot, hide-raw)
  models/    ONNX models (opencv_zoo: PP-HumanSeg, YuNet, MediaPipe palm+hand)
  assets/    emoji sprites for reactions
  install.sh
```

## Install

Either way you end up with a camera icon in the bar; pick **"Iris Camera"**
in any app. `./install.sh --uninstall` reverses everything.

```bash
# as an Omarchy plugin (clones into ~/.config/omarchy/plugins/alanfortlink.iris):
omarchy plugin add https://github.com/<you>/camera_effects.git --enable
#   the bar shows a camera icon; open it and click "Install (build daemon)": it installs the
#   build deps if missing, builds irisd, installs the user half under ~/.local, and asks for
#   your password once to create the virtual camera device (and again per "hide raw" toggle).

# or from a checkout:
./install.sh          # same thing non-interactively (asks for the password via sudo/pkexec)
```

`omarchy plugin update alanfortlink.iris` fast-forwards the checkout; run `./install.sh --no-root`
(or the panel's Install button) afterwards to rebuild the daemon.

Requirements (all in Omarchy already): `opencv`, `onnxruntime`, `pipewire`,
`v4l2loopback-dkms` (+ `v4l2loopback-utils`, installed by setup), `qt6-multimedia`.

## How it works

- **irisd** opens the real camera only while some app holds the virtual camera
  (it watches opens/closes on the device with inotify), so the camera light is
  off when nothing uses it — like macOS. Frames go to `/dev/video20` (a
  v4l2loopback device with `exclusive_caps=1`, primed so it advertises capture
  formats while idle) **and** to a native PipeWire `Video/Source` node with
  `media.role=Camera` (for portal-based apps: OBS "Camera (PipeWire)",
  Firefox/Chromium with PipeWire camera enabled).
- **The device at boot** is created by a tiny systemd unit installed by
  `iris-setup install` (`v4l2loopback-ctl add -n "Iris Camera" -x 1`).
- **The panel** talks to the daemon over a unix socket
  (`$XDG_RUNTIME_DIR/iris/ctl.sock`, newline JSON). The preview is a
  `QtMultimedia` `Camera` reading the loopback (so it shows the processed feed;
  it is also what wakes the daemon while the panel is open).
- **Hide raw camera** (switches in the panel, ask for your password): a udev rule
  takes the physical webcams away from your user (group `camerad`) and installs a
  root-owned setgid copy of the daemon at `/usr/local/lib/iris/irisd`,
  so apps can only see "Iris Camera". With several cameras you get one switch
  for all of them plus one per USB camera (matched by vendor/product/serial).
  Toggling off restores everything. This is a courtesy boundary for well-behaved
  apps (they no longer list the raw camera), not protection against malware
  running as your user. The privileged half is `iris-setup`; `install`
  puts a root-owned copy in `/usr/local/lib/iris/` which the shell uses
  for every later password prompt (re-run `./install.sh` to update it).
- With several cameras the panel has a picker for the source and a "Same effects
  on every camera" switch; off, each camera keeps its own effect settings.
- Settings persist per user in `~/.config/iris/config.json`.

## CLI

```bash
irisd status                       # JSON state (running, consumers, fps, settings…)
irisd set portrait=true portraitIntensity=0.7 centerStage=true
irisd react hearts                 # play a reaction now
irisd camera <bus-or-/dev/path>    # choose the physical camera (or a video/image file, handy for testing)
irisd preview on|off               # keep the pipeline running without a consumer
irisd profile on                   # per-stage timings in `status`
omarchy-shell alanfortlink.iris toggle    # open/close the panel (bind it to a key)
```

## Publishing (Omarchy shell plugin conventions)

The repo root *is* the plugin: `manifest.json` (id `alanfortlink.iris`, kinds `service` +
`bar-widget`, entry points under `plugin/`), no symlinks, `omarchy plugin validate .`
passes. Push it to a public git repo and list it at omarchyplugins.com. Bump
`version` in `manifest.json` for releases; users update with `omarchy plugin update`.

## Notes / limits

- Output is 1280x720 @ 30 (`~/.config/iris/config.json` → `output`). The
  source is captured at the output size (720p MJPEG) and switched to `capture`
  (1920x1080 by default) only while Center Stage is on, so it can zoom without
  upscaling.
- Performance: the daemon is tuned for weak dual-core laptops (2 OpenCV threads,
  one shared 2-thread ONNX Runtime pool, 8-bit compositing, downscale pyramid
  shared by all detectors, decode on its own thread). It measures the whole
  per-frame cost against 80% of the frame period and steps through three
  quality tiers (`tier` in `status`: 0 = full; 1 = segmentation every 2nd frame,
  slower gesture/face cadence, single blur pass; 2 = every 3rd frame, portrait
  at 1/8 scale) with hysteresis; `procMs` in `status` is that per-frame cost.
- Chromium/Electron/Zoom read cameras through V4L2 (the loopback); the PipeWire
  node matters only for portal-based apps and for the "hide raw" mode.
- Gesture triggers: hold the gesture ~0.7 s away from your face; 4 s cooldown.
- Cameras that only work through libcamera (Intel IPU6 laptops) are not read yet
  (the daemon uses V4L2 directly).
- Presenter Overlay is not implemented.
