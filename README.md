# Camera Effects

macOS-style camera effects for Omarchy: Center Stage, Portrait, Studio Light, backgrounds, filters, reactions and a privacy shutter, on any webcam, in every app.

It runs as a virtual camera called **Camera Effects** that you pick in Zoom, Meet, OBS or anything else, and a panel in the bar where you switch things on and off while watching a live preview.

> Tested only on **Omarchy 4** (Arch Linux, Hyprland, omarchy-shell).
>
> Video walkthrough: coming soon.

## What you get

- **Center Stage** keeps you framed as you move, with an intensity slider for how tightly it follows.
- **Portrait** blurs the background, **Studio Light** brightens your face; both have intensity sliders.
- **Background** replacement with a colour or your own image.
- **Filters** (mono, sepia, warm, cool, vivid, soft, sharpen, vintage) and **Effects** (sunglasses, top hat, crown, cat, halo and more).
- **Reactions** such as hearts, confetti, balloons and fireworks, played with a click or by holding a hand gesture at the camera.
- Manual framing: zoom, drag-to-pan, fit mode, rotate and mirror.
- **Snapshots** with a 3·2·1 countdown, saved to `~/Pictures/Camera Effects/` and copied to the clipboard.
- **Privacy**: block the camera with a card, image or video, or hide the raw webcam so apps only see Camera Effects.
- Several webcams? Pick the source in the panel and keep shared or per-camera settings.
- Everything runs on the CPU, locally. Nothing leaves your machine.

## Install

```bash
omarchy plugin add https://github.com/alanfortlink/camera-effects.git --enable
```

A camera icon appears in the bar. Open it and click **Install (build daemon)**. That installs any missing packages, builds the daemon (a minute or two) and asks for your password once to create the virtual camera.

Then pick **Camera Effects** as the camera in any app.

## Update and uninstall

Update:

```bash
omarchy plugin update alanfortlink.camera-effects
```

That is all: the plugin notices the new code, rebuilds the daemon in the background and restarts it. You are asked for your password only when the root-owned parts changed. (Manual equivalent: run `~/.config/omarchy/plugins/alanfortlink.camera-effects/install.sh`.)

Uninstall, in this order:

```bash
cd ~/.config/omarchy/plugins/alanfortlink.camera-effects && ./install.sh --uninstall
omarchy plugin remove alanfortlink.camera-effects
```

If the plugin folder is already gone, the root part can still be removed with `sudo /usr/local/lib/camera-effects/camera-effects-setup uninstall`.

## Using it

Click the bar icon to open the panel (right-click toggles Portrait). The bar icon lights up while an app is using the camera.

| Panel section | What it does |
| --- | --- |
| Preview | Live view of what apps see. Drag to pan, scroll to zoom. The shutter button takes a snapshot after a 3·2·1 countdown. |
| Video | Center Stage, Zoom, Fit, Portrait, Studio Light, Background, Filter, Effects, Reactions, Rotate, Mirror preview, Mirror output. |
| Privacy | Block camera and Hide raw camera (see below). |
| Footer | Which apps are using the camera right now, and the output resolution. |

Reactions play when you click one, or when you hold the matching gesture: heart hands, thumbs up, thumbs down, a peace sign, two peace signs, two thumbs up, two thumbs down, or two rock signs.

Handy commands: `omarchy-shell alanfortlink.camera-effects toggle` opens the panel (bind it to a key) and `omarchy-shell alanfortlink.camera-effects snap` takes a snapshot.

The daemon also has a small CLI, `camera-effects-server`, with the setting keys matching the JSON that `status` prints:

| Verb | Effect |
| --- | --- |
| `status` | Print the current state and settings as JSON. |
| `set key=value…` | Change settings, e.g. `set portrait=true zoom=1.5`. |
| `react NAME` | Play a reaction (`hearts`, `confetti`, …). |
| `camera BUS` | Switch to another webcam. |
| `snap` | Take a snapshot and print the PNG path. |
| `preview on` | Hold the pipeline open while the command runs. |

## Privacy

**Block camera** turns the webcam off and sends apps a "camera off" card, a still image or a video instead. Apps keep working; they just don't see you.

**Hide raw camera** makes the physical webcam invisible to apps, so Camera Effects is the only camera they can pick. It can hide all webcams or one at a time. This changes udev rules, so it asks for your password. It is a courtesy boundary for well-behaved apps, not protection against malware or anyone with root.

## How it works

A small daemon (`camera-effects-server`, C++ with OpenCV and a few small ONNX models) opens the real webcam only while an app is using the virtual one, processes each frame and publishes it as a v4l2loopback device and a PipeWire camera node.

The panel talks to the daemon over a unix socket in `$XDG_RUNTIME_DIR/camera-effects/`, and settings live in `~/.config/camera-effects/config.json`.

Root is used only for the loopback device (created at boot by a systemd unit), the hide-raw udev rules and, when hiding is on, a root-owned copy of the daemon in `/usr/local/lib/camera-effects/`.

## Troubleshooting

- No bar icon: `omarchy plugin enable alanfortlink.camera-effects`, or `omarchy-restart-shell`.
- Panel says "Needs setup" or v4l2loopback won't load: reboot after a kernel update, or check `dkms status`.
- Apps don't list the camera: check `systemctl status camera-effects-device` and `v4l2loopback-ctl list`, then re-run `./install.sh`.
- Not supported yet: cameras that only work through libcamera (Intel IPU6), and Presenter Overlay.

## Disclaimer

This is a hobby project, provided **as is**, with no warranty of any kind. You use it entirely at your own risk. It runs unsandboxed inside your shell, opens your camera, installs a systemd unit and udev rules and (if you enable hide-raw) a privileged helper; read the code before you trust it, and don't run it where that would be a problem. The author is not responsible for any damage, data loss, privacy incident, embarrassing video call, or anything else that results from using this software.

## License

MIT. Third-party model and asset licenses are listed in `THIRD_PARTY_NOTICES.md`.
