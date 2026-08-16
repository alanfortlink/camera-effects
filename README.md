# Omarchy Camera — system-wide camera effects

macOS-style camera effects for Omarchy: a background daemon reads your webcam,
applies effects, and publishes the result as a virtual webcam called
**"Omarchy Camera"** that every app (Chromium, Zoom, Discord, OBS, Firefox…)
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
  daemon/    camfxd — C++ daemon (V4L2 capture → effects → v4l2loopback + native PipeWire node)
  plugin/    Quickshell plugin for omarchy-shell (Service.qml, Panel.qml, Preview.qml)
  scripts/   omarchy-camera-setup — the privileged part (device at boot, hide-raw)
  models/    ONNX models (opencv_zoo: PP-HumanSeg, YuNet, MediaPipe palm+hand)
  assets/    emoji sprites for reactions
  install.sh
```

## Install

Either way you end up with a camera icon in the bar; pick **"Omarchy Camera"**
in any app. `./install.sh --uninstall` reverses everything.

```bash
# as an Omarchy plugin (clones into ~/.config/omarchy/plugins/tank.camera):
omarchy plugin add <this repo> --enable
#   then click "Install (build daemon)" in the panel — it builds camfxd, installs the
#   user half and asks for your password once to create the virtual camera device.

# or from a checkout:
./install.sh          # builds, installs to ~/.local, enables the plugin, creates the device (password)
```

Requirements (all in Omarchy already): `opencv`, `onnxruntime`, `pipewire`,
`v4l2loopback-dkms` (+ `v4l2loopback-utils`, installed by setup), `qt6-multimedia`.

## How it works

- **camfxd** opens the real camera only while some app holds the virtual camera
  (it watches opens/closes on the device with inotify), so the camera light is
  off when nothing uses it — like macOS. Frames go to `/dev/video20` (a
  v4l2loopback device with `exclusive_caps=1`, primed so it advertises capture
  formats while idle) **and** to a native PipeWire `Video/Source` node with
  `media.role=Camera` (for portal-based apps: OBS "Camera (PipeWire)",
  Firefox/Chromium with PipeWire camera enabled).
- **The device at boot** is created by a tiny systemd unit installed by
  `omarchy-camera-setup install` (`v4l2loopback-ctl add -n "Omarchy Camera" -x 1`).
- **The panel** talks to the daemon over a unix socket
  (`$XDG_RUNTIME_DIR/omarchy-camera/ctl.sock`, newline JSON). The preview is a
  `QtMultimedia` `Camera` reading the loopback (so it shows the processed feed;
  it is also what wakes the daemon while the panel is open).
- **Hide raw camera** (switches in the panel, ask for your password): a udev rule
  takes the physical webcams away from your user (group `camerad`) and installs a
  root-owned setgid copy of the daemon at `/usr/local/lib/omarchy-camera/camfxd`,
  so apps can only see "Omarchy Camera". With several cameras you get one switch
  for all of them plus one per USB camera (matched by vendor/product/serial).
  Toggling off restores everything. This is a courtesy boundary for well-behaved
  apps (they no longer list the raw camera), not protection against malware
  running as your user. The privileged half is `omarchy-camera-setup`; `install`
  puts a root-owned copy in `/usr/local/lib/omarchy-camera/` which the shell uses
  for every later password prompt (re-run `./install.sh` to update it).
- With several cameras the panel has a picker for the source and a "Same effects
  on every camera" switch; off, each camera keeps its own effect settings.
- Settings persist per user in `~/.config/omarchy/camera.json`.

## CLI

```bash
camfxd status                       # JSON state (running, consumers, fps, settings…)
camfxd set portrait=true portraitIntensity=0.7 centerStage=true
camfxd react hearts                 # play a reaction now
camfxd camera <bus-or-/dev/path>    # choose the physical camera (or a video/image file, handy for testing)
camfxd preview on|off               # keep the pipeline running without a consumer
camfxd profile on                   # per-stage timings in `status`
omarchy-shell tank.camera toggle    # open/close the panel (bind it to a key)
```

## Notes / limits

- Output is 1280x720 @ 30 (`~/.config/omarchy/camera.json` → `output`), source
  captured at 1920x1080 MJPEG when possible so Center Stage can zoom without
  upscaling.
- Chromium/Electron/Zoom read cameras through V4L2 (the loopback); the PipeWire
  node matters only for portal-based apps and for the "hide raw" mode.
- Gesture triggers: hold the gesture ~0.7 s away from your face; 4 s cooldown.
- Cameras that only work through libcamera (Intel IPU6 laptops) are not read yet
  (the daemon uses V4L2 directly).
- Presenter Overlay is not implemented.
