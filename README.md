# Iris

macOS-style camera effects for Omarchy: Center Stage, Portrait, Studio Light,
backgrounds and hand-gesture reactions on any webcam, published as a virtual
camera called **"Iris Camera"** that every app can pick.

> Tested only on **Omarchy 4** (Arch Linux, Hyprland, omarchy-shell).
> Video walkthrough: coming soon.

| Bar icon (lit while an app uses the camera) | Panel |
|---|---|
| ![bar](docs/bar.png) | ![panel](docs/panel.png) |

| Effects on | Reaction |
|---|---|
| ![effects](docs/effects.png) | ![reaction](docs/reaction.png) |

## Install

```bash
omarchy plugin add https://github.com/alanfortlink/camera-effects.git --enable
```

A camera icon appears in the bar. Open it and click **Install (build daemon)**:
it installs missing packages (`onnxruntime`, `v4l2loopback-dkms`; a password
prompt if any is missing), builds the daemon into `~/.local` (a minute or two;
log in `~/.cache/iris/install.log`), then asks for your password once more to
create the virtual camera now and at every boot. Then pick **Iris Camera** in
Chromium, Zoom, Discord, OBS, Firefox… From a terminal, `./install.sh` in the
checkout does the same.

**Update**: `omarchy plugin update alanfortlink.iris`, then
`~/.config/omarchy/plugins/alanfortlink.iris/install.sh` (rebuilds and refreshes
the root-owned copies; one prompt). If the daemon stops starting after a system
update, the panel shows the error and a **Rebuild daemon** button.

**Uninstall** — in this order (the second step deletes `install.sh`):

```bash
cd ~/.config/omarchy/plugins/alanfortlink.iris && ./install.sh --uninstall  # one prompt; don't cancel it (it un-hides your webcams)
omarchy plugin remove alanfortlink.iris
# checkout already gone?  sudo /usr/local/lib/iris/iris-setup uninstall
```

## Using it

- **Effects**: Center Stage (auto-framing), Portrait (blur, with intensity),
  Studio Light (with intensity), Background (color or image), Mirror.
- **Reactions**: hearts, thumbs up/down, balloons, confetti, fireworks, rain,
  lasers. Click one, or hold a gesture ~1 s away from your face: heart hands,
  thumbs up/down, peace sign, two peace signs, two thumbs up/down, two rock
  signs (4 s cooldown).
- **Several cameras**: a source picker and a "Same effects on every camera"
  switch; off, each camera keeps its own settings.
- **Hide raw camera from apps** (Privacy section, password on each toggle): USB
  webcams are taken away from your user so apps only see "Iris Camera" (one
  switch for all, plus one per camera); toggling off restores them. This keeps
  well-behaved apps off the raw feed; it is not protection against malware
  running as you.
- Right-click the bar icon to toggle Portrait. Bind
  `omarchy-shell alanfortlink.iris toggle` to a key to open the panel.

## How it works

`irisd` (C++, OpenCV + small ONNX models, CPU only, ~20 ms per 720p frame on a
desktop) opens the real camera only while an app holds the virtual one, so the
camera light stays off otherwise. Frames go to a v4l2loopback device
(`/dev/video20`, created at boot by a small systemd unit) and to a PipeWire
camera node for portal-based apps. The panel talks to the daemon over
`$XDG_RUNTIME_DIR/iris/ctl.sock`; settings live in `~/.config/iris/config.json`.
The only privileged parts are `iris-setup` (device at boot, hide rules) and, when
hiding is on, a root-owned copy of the daemon, both in `/usr/local/lib/iris/`.
Nothing leaves your machine.

## CLI

```bash
irisd status                          # JSON state (consumers, fps, settings…)
irisd set portrait=true portraitIntensity=0.7 centerStage=true
irisd react hearts                    # play a reaction now
irisd camera <bus-or-/dev/path>       # pick the physical camera (or a video file)
irisd preview on|off                  # keep the pipeline running without a consumer
```

## Troubleshooting and limits

- No icon: `omarchy plugin enable alanfortlink.iris` (or `omarchy-restart-shell`).
- "Needs setup": click **Set up virtual camera**. If it says v4l2loopback is not
  loadable, reboot (kernel updated) or check `dkms status`.
- App doesn't list Iris Camera: `systemctl status iris-camera-device`,
  `v4l2loopback-ctl list`; re-run `./install.sh`.
- Output is 1280x720 @ 30 (editable in `~/.config/iris/config.json`); cameras
  that only work through libcamera (Intel IPU6) are not read; no Presenter Overlay.
- Developing: `./install.sh` from any checkout symlinks it into
  `~/.config/omarchy/plugins`; remove that symlink before `omarchy plugin add`.

## License

MIT — third-party model and asset licenses in `THIRD_PARTY_NOTICES.md`.
