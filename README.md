# Camera Effects

macOS-style camera effects for Omarchy: Center Stage, Portrait, Studio Light,
backgrounds, colour filters, fun face filters, hand-gesture reactions and a
privacy shutter on any webcam, published as a virtual camera called
**"Camera Effects"** that every app can pick.

> Tested only on **Omarchy 4** (Arch Linux, Hyprland, omarchy-shell).
> Video walkthrough: coming soon.


## Install

```bash
omarchy plugin add https://github.com/alanfortlink/camera-effects.git --enable
```

A camera icon appears in the bar. Open it and click **Install (build daemon)**:
it installs missing packages (`onnxruntime`, `v4l2loopback-dkms`; a password
prompt if any is missing), builds the daemon into `~/.local` (a minute or two;
log in `~/.cache/camera-effects/install.log`), then asks for your password once more to
create the virtual camera now and at every boot. Then pick **Camera Effects** in
Chromium, Zoom, Discord, OBS, Firefox… From a terminal, `./install.sh` in the
checkout does the same.

**Update**: `omarchy plugin update alanfortlink.camera-effects`, then
`~/.config/omarchy/plugins/alanfortlink.camera-effects/install.sh` (rebuilds and refreshes
the root-owned copies; one prompt). If the daemon stops starting after a system
update, the panel shows the error and a **Rebuild daemon** button.

**Uninstall** — in this order (the second step deletes `install.sh`):

```bash
cd ~/.config/omarchy/plugins/alanfortlink.camera-effects && ./install.sh --uninstall  # one prompt; don't cancel it (it un-hides your webcams)
omarchy plugin remove alanfortlink.camera-effects
# checkout already gone?  sudo /usr/local/lib/camera-effects/camera-effects-setup uninstall
```

## Using it

- **Video**: Center Stage (auto-framing: head and shoulders, up to 3x on a
  1080p camera), Portrait (blur, with intensity), Studio Light (with
  intensity), Background (color or image), Rotate (90/180/270), Mirror.
- **Framing**: Zoom (1-4x; slider, or scroll on the preview), pan (drag the
  preview), Fit (Cover crops to 16:9, Contain letterboxes, Stretch ignores
  the aspect). With Center Stage on, the zoom is its minimum and it pans on
  its own. The same controls frame the block placeholder image or video.
- **Filter**: a colour look — Mono, Sepia, Warm, Cool, Vivid, Soft, Sharpen,
  Vintage (about 1 ms per frame).
- **Effects**: face accessories that follow your head (up to four faces) —
  Sunglasses, Glasses, Top hat, Crown, Cat, Halo, Headphones, Flowers.
- **Reactions**: hearts, thumbs up/down, balloons, confetti, fireworks, rain,
  lasers. Click one, or hold a gesture ~1 s away from your face: heart hands,
  thumbs up/down, peace sign, two peace signs, two thumbs up/down, two rock
  signs (4 s cooldown).
- **Snapshot**: the shutter button on the preview (or
  `omarchy-shell alanfortlink.camera-effects snap`, or `camera-effects-server
  snap`) counts 3 · 2 · 1, then saves what apps see, at full output size, as
  `~/Pictures/Camera Effects/YYYY-MM-DD_HH-MM-SS.png`, copies it to the
  clipboard and notifies.
- **Several cameras**: a source picker and a "Same effects on every camera"
  switch; off, each camera keeps its own settings.
- **Block camera** (Privacy section): the webcam stays closed (light off) even
  while apps use Camera Effects; they get the built-in "Camera paused" card, or an
  image or a looped video of your choice, instead. Global, not per camera.
- **Hide raw camera from apps** (Privacy section, password on each toggle): USB
  webcams are taken away from your user so apps only see "Camera Effects" (one
  switch for all, plus one per camera); toggling off restores them. This keeps
  well-behaved apps off the raw feed; it is not protection against malware
  running as you.
- Right-click the bar icon to toggle Portrait. Bind
  `omarchy-shell alanfortlink.camera-effects toggle` to a key to open the panel.

## How it works

`camera-effects-server` (C++, OpenCV + small ONNX models, CPU only, ~20 ms per 720p frame on a
desktop) opens the real camera only while an app holds the virtual one, so the
camera light stays off otherwise. Frames go to a v4l2loopback device
(`/dev/video20`, created at boot by a small systemd unit) and to a PipeWire
camera node for portal-based apps. The panel talks to the daemon over
`$XDG_RUNTIME_DIR/camera-effects/ctl.sock`; its preview is the processed feed as
the daemon serves it (a small JPEG next to the socket, refreshed ~15 times a
second while the panel is open), so it stays live while apps stream. Settings
live in `~/.config/camera-effects/config.json`.
The only privileged parts are `camera-effects-setup` (device at boot, hide rules) and, when
hiding is on, a root-owned copy of the daemon, both in `/usr/local/lib/camera-effects/`.
Nothing leaves your machine.

## CLI

```bash
camera-effects-server status                          # JSON state (consumers, fps, settings…)
camera-effects-server set portrait=true portraitIntensity=0.7 centerStage=true
camera-effects-server set filter=vintage fun=sunglasses
camera-effects-server set zoom=1.5 panX=0.3 panY=-0.2 rotate=90 fit=contain   # framing (pan -1..1 within the room the zoom leaves)
camera-effects-server set block=true blockSource=/path/to/image-or-video   # blockSource= for the built-in card
camera-effects-server set blockZoom=2 blockPanX=1 blockFit=cover           # frame the placeholder
camera-effects-server react hearts                    # play a reaction now
camera-effects-server snap                            # save the next output frame as a PNG (prints the path)
camera-effects-server camera <bus-or-/dev/path>       # pick the physical camera (or a video file)
camera-effects-server preview on                      # run the pipeline without a consumer (and write preview.jpg) while this command runs
```

## Troubleshooting and limits

- No icon: `omarchy plugin enable alanfortlink.camera-effects` (or `omarchy-restart-shell`).
- "Needs setup": click **Set up virtual camera**. If it says v4l2loopback is not
  loadable, reboot (kernel updated) or check `dkms status`.
- App doesn't list Camera Effects: `systemctl status camera-effects-device`,
  `v4l2loopback-ctl list`; re-run `./install.sh`.
- Output is 1280x720 @ 30 (editable in `~/.config/camera-effects/config.json`); cameras
  that only work through libcamera (Intel IPU6) are not read; no Presenter Overlay.
- Developing: `./install.sh` from any checkout symlinks it into
  `~/.config/omarchy/plugins`; remove that symlink before `omarchy plugin add`.

## License

MIT — third-party model and asset licenses in `THIRD_PARTY_NOTICES.md`.
