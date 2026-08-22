# Camera Effects

macOS-style camera **and microphone** effects for Omarchy: Center Stage, Portrait, Studio Light, backgrounds, filters, reactions and a privacy shutter on any webcam, Voice Isolation, auto level and voice changers on any microphone, in every app.

It runs as a virtual camera called **Camera Effects** and a virtual microphone called **Microphone Effects** that you pick in Zoom, Meet, OBS or anything else, and a panel in the bar where you switch things on and off while watching a live preview

> Tested only on **Omarchy 4** (Arch Linux, Hyprland, omarchy-shell).



https://github.com/user-attachments/assets/1a460e2d-d732-4391-97cc-6155982c594f



Thanks to [@prettyletto](https://github.com/prettyletto) for sending me this video.


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
- **Microphone effects**: pick the mic, set its volume (±18 dB, effects or not, or **Auto** to set it from what the mic actually hears), then a preset (Clean, Voice, Meeting, Podcast) or the individual effects: Voice Isolation (spectral noise suppression, ~40 ms of delay while it is on), a noise gate, auto level, a de-esser, a rumble filter, a 50/60 Hz hum filter, tone presets (warm, bright, clarity, podcast, telephone), voice changers (deep, chipmunk, robot, alien, megaphone, monster) and space (room, hall, cathedral, echo, underwater), with Before/After level meters.
- **Listen to yourself**: the Mic tab can play the processed microphone through your speakers, so you hear what apps get (use headphones). The switch is remembered, and the playback only runs while the tab is open.
- **Mute** the microphone (apps get silence and the mic is released), and hide the real microphones so apps only see Microphone Effects.
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
| Video | Camera, Center Stage, Zoom, Fit, Rotate, Mirror preview, Mirror output, and Reset (this camera back to the defaults). |
| Look | Portrait, Studio Light, Background (a colour, an image or a looped video) and Filter. |
| Fun | Effects (sunglasses, mask, hats, cat ears, blur, pixelate…), Ambience (endless rain, snow, sparkles, confetti or bubbles) and Reactions. |
| Mic | Microphone, Before/After meters, Mute, Listen, Volume (+ Auto), a Microphone effects card with presets, then two folding sections — Cleanup (Voice isolation, Noise gate, Auto level, De-esser, Rumble filter, Hum filter) and Character (Tone, Voice, Space chips) — and finally Hide raw microphones. The camera preview is off on this tab, and the mic is opened only while the tab is on screen (for the meter). |
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
| `mic NODE` | Switch to another microphone (a PipeWire `node.name`; empty = the system default). |
| `micset key=value…` | Change microphone settings, e.g. `micset voiceIsolation=true voice=deep muted=false listen=true`. |
| `micreset` | Microphone effects back to the defaults. |
| `snap` | Take a snapshot and print the PNG path. |
| `preview on` | Hold the pipeline open while the command runs. |

Mute is also on IPC, so it can go on a key without opening the panel:

```
bind = , code:121, exec, omarchy-shell alanfortlink.camera-effects micMute   # XF86AudioMicMute
```

`micMuteOn` / `micMuteOff` are there too, for push-to-talk bindings.

## Privacy

**Block camera** turns the webcam off and sends apps a "camera off" card, a still image or a video instead. Apps keep working; they just don't see you.

**Hide raw camera** makes the physical webcam invisible to apps, so Camera Effects is the only camera they can pick. It can hide all webcams or one at a time. This changes udev rules, so it asks for your password. It is a courtesy boundary for well-behaved apps, not protection against malware or anyone with root.

**Listen to this microphone** plays the processed microphone to your default sink, so you can check what you sound like before a call. It is on by default and the switch is remembered, but the playback only runs while the Mic tab is open — leaving the tab or closing the panel stops it, so the speakers are never left echoing. It stays off while the microphone is muted. With a Bluetooth headset the device switches to the headset profile while you listen (that is the only profile in which its microphone works), so the playback quality drops until you switch it off.

**Mute microphone** releases the microphone entirely (the recording light goes out) and feeds apps silence.

**Hide raw microphone** does the same job as the camera one, without a password: it drops a WirePlumber script and config fragment in your own `~/.config/wireplumber` / `~/.local/share/wireplumber`, which takes the right to *see* the real microphones away from every client except WirePlumber and Camera Effects itself. The rule only applies while "Microphone Effects" is actually there — if the daemon stops, the real microphones come back within a second, so you are never left with no microphone at all. Applying it restarts your WirePlumber (a second of silence) and makes Microphone Effects the default microphone. Turning it off restarts WirePlumber again, leaving any per-microphone rules in place (uninstalling clears everything). Same caveat: a courtesy boundary, not a sandbox.

## How it works

A small daemon (`camera-effects-server`, C++ with OpenCV and a few small ONNX models) opens the real webcam only while an app is using the virtual one, processes each frame and publishes it as a v4l2loopback device and a PipeWire camera node.

The same daemon publishes the microphone: a PipeWire `Audio/Source` node apps pick, fed by a capture stream on the real mic in the same link group (so there is no drift between them), plus a playback stream while "Listen to this microphone" is on, and — for a Bluetooth mic — a small fourth stream that asks WirePlumber for the headset profile. The real microphone is opened only while an app, the level meter or the listen switch needs it.

The panel talks to the daemon over a unix socket in `$XDG_RUNTIME_DIR/camera-effects/`, and settings live in `~/.config/camera-effects/config.json`.

Root is used only for the loopback device (created at boot by a systemd unit), the hide-raw udev rules and, when hiding is on, a root-owned copy of the daemon in `/usr/local/lib/camera-effects/`.

## Troubleshooting

- No bar icon: `omarchy plugin enable alanfortlink.camera-effects`, or `omarchy-restart-shell`.
- Panel says "Needs setup" or v4l2loopback won't load: reboot after a kernel update, or check `dkms status`.
- Apps don't list the camera: check `systemctl status camera-effects-device` and `v4l2loopback-ctl list`, then re-run `./install.sh`.
- Bluetooth headset mic: the daemon asks WirePlumber for the headset (HSP/HFP) profile while it reads one, so your headphones drop to call quality for as long as the microphone is open — including while you are only listening or watching the level meter. It goes back to A2DP a couple of seconds after the mic is released.
- Microphone Effects missing from an app: check `pactl list short sources | grep camera-effects-mic`, and remember that apps started before the daemon may need reopening.
- No microphone in apps that start before the panel does: hiding the real microphones is a WirePlumber setting, so it applies from login, while "Microphone Effects" only exists once the shell (and with it the daemon) is up. Reopen the app, or leave hiding off if you have apps that autostart.
- Audio devices gone after turning "Hide raw microphone" on: the panel puts the rules back if WirePlumber refuses to start, but you can always remove `~/.config/wireplumber/wireplumber.conf.d/71-camera-effects-hide-mics.conf` and run `systemctl --user restart wireplumber`.
- Not supported yet: cameras that only work through libcamera (Intel IPU6), and Presenter Overlay.

## Disclaimer

This is a hobby project, provided **as is**, with no warranty of any kind. You use it entirely at your own risk. It runs unsandboxed inside your shell, opens your camera, installs a systemd unit and udev rules and (if you enable hide-raw) a privileged helper; read the code before you trust it, and don't run it where that would be a problem. The author is not responsible for any damage, data loss, privacy incident, embarrassing video call, or anything else that results from using this software.

## License

MIT. Third-party model and asset licenses are listed in `THIRD_PARTY_NOTICES.md`.
