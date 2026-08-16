# Camera Effects

macOS-style camera effects for Omarchy — Center Stage, Portrait, Studio Light,
backgrounds, colour filters, face effects, hand-gesture reactions, a privacy
shutter and snapshots — on any webcam, published as a virtual camera called
**"Camera Effects"** that every app can pick.

> Tested only on **Omarchy 4** (Arch Linux, Hyprland, omarchy-shell).
> Video walkthrough: coming soon.

## Install

```bash
omarchy plugin add https://github.com/alanfortlink/camera-effects.git --enable
```

Open the new camera icon in the bar and click **Install (build daemon)**. It
installs missing packages, builds the daemon (a minute or two) and asks for your
password to create the virtual camera. Then pick **Camera Effects** in any app.

- Update: `omarchy plugin update alanfortlink.camera-effects`, then run
  `~/.config/omarchy/plugins/alanfortlink.camera-effects/install.sh`.
- Uninstall, in this order: `./install.sh --uninstall` in that folder, then
  `omarchy plugin remove alanfortlink.camera-effects`
  (checkout already gone? `sudo /usr/local/lib/camera-effects/camera-effects-setup uninstall`).

## Using it

Everything is in the panel: video controls (Center Stage, Portrait, Studio
Light, Background, Zoom/pan/Fit/Rotate/Mirror), Filter, Effects, Reactions
(click, or hold a gesture), a shutter button (3·2·1 → PNG in
`~/Pictures/Camera Effects/`, copied to the clipboard), Block camera (apps get a
card, image or video; the webcam stays off) and Hide raw camera (apps only see
"Camera Effects"; asks for your password; a courtesy boundary, not malware
protection). Several cameras: pick the source and choose shared or per-camera
settings.

`omarchy-shell alanfortlink.camera-effects toggle` opens the panel (bind it);
`… snap` takes a snapshot; right-click the bar icon toggles Portrait.

## How it works

`camera-effects-server` (C++, OpenCV + small ONNX models, CPU only) opens the
real camera only while an app uses the virtual one. Frames go to a v4l2loopback
device (created at boot by a small systemd unit) and a PipeWire camera node. The
panel talks to it over a unix socket in `$XDG_RUNTIME_DIR/camera-effects/`;
settings live in `~/.config/camera-effects/config.json`. Root is used only for
the device at boot, the hide-raw udev rules and (when hiding is on) a root-owned
copy of the daemon in `/usr/local/lib/camera-effects/`. Nothing leaves your
machine.

`camera-effects-server status|set key=value…|react NAME|snap|camera BUS|preview on`
is the CLI (settings keys are the JSON keys in `status`).

## Troubleshooting

- No icon: `omarchy plugin enable alanfortlink.camera-effects` or `omarchy-restart-shell`.
- "Needs setup" / v4l2loopback not loadable: reboot after a kernel update, or check `dkms status`.
- App doesn't list the camera: `systemctl status camera-effects-device`, `v4l2loopback-ctl list`, re-run `./install.sh`.
- Not supported yet: cameras that only work through libcamera (Intel IPU6), Presenter Overlay.

## License

MIT — third-party model and asset licenses in `THIRD_PARTY_NOTICES.md`.
