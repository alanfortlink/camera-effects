#!/bin/bash
# Build and install the Camera Effects plugin for the current user.
#   ./install.sh            build daemon, install files, enable the shell plugin, set up the device (asks for password)
#   ./install.sh --no-root  everything except the root part (prints the command to run instead;
#                           only missing packages still prompt, through pkexec)
#   ./install.sh --uninstall
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=$HOME/.local/lib/camera-effects
DATA=$HOME/.local/share/camera-effects
BIN=$HOME/.local/bin
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/camera-effects
PLUGIN=$HOME/.config/omarchy/plugins/alanfortlink.camera-effects
ID=alanfortlink.camera-effects
MODE=${1:-}
SETUP=$HERE/scripts/camera-effects-setup

privileged() {  # run the setup script as root: silently if sudo allows it, else via the polkit prompt
  if sudo -n true 2>/dev/null; then sudo "$SETUP" "$@"; else pkexec "$SETUP" "$@"; fi
}

if [[ $MODE == --uninstall ]]; then
  # The root half first (it un-hides the webcams and removes the device); if it did not
  # run, the user half stays too, so the user can retry rather than lose the tools.
  if ! privileged uninstall; then
    echo "error: the root part was NOT removed (cancelled or failed); your webcams may still be hidden." >&2
    echo "  retry: ./install.sh --uninstall    or, without the checkout: sudo /usr/local/lib/camera-effects/camera-effects-setup uninstall" >&2
    exit 1
  fi
  omarchy-plugin-disable "$ID" >/dev/null 2>&1 || true
  rm -rf "$LIB" "$DATA" "$BIN/camera-effects-server" "$CACHE"
  [[ -L $PLUGIN ]] && rm -f "$PLUGIN"   # dev symlink; a real checkout is removed with `omarchy plugin remove alanfortlink.camera-effects`
  echo "uninstalled (config left in ~/.config/camera-effects/config.json). Now run: omarchy plugin remove $ID"
  exit 0
fi

# Build/runtime dependencies (all in the Omarchy/Arch repos).
missing=()
for p in gcc make pkgconf opencv onnxruntime pipewire qt6-multimedia v4l2loopback-dkms dkms; do
  pacman -Q "$p" >/dev/null 2>&1 || missing+=("$p")
done
# The dkms module needs the running kernel's headers; pick the package for this kernel flavour.
krel=$(uname -r)
if [[ ! -e /usr/lib/modules/$krel/build ]]; then
  flavour=linux
  case "$krel" in *-zen*) flavour=linux-zen;; *-lts*) flavour=linux-lts;; *-hardened*) flavour=linux-hardened;; *-rt-lts*) flavour=linux-rt-lts;; *-rt*) flavour=linux-rt;; esac
  pacman -Q "$flavour" >/dev/null 2>&1 && missing+=("${flavour}-headers")
fi
if ((${#missing[@]})); then
  echo "› installing missing packages: ${missing[*]} (password prompt)"
  # --no-root never uses sudo (a cached ticket must not change what it does): pkexec asks.
  if [[ $MODE != --no-root ]] && sudo -n true 2>/dev/null; then sudo pacman -S --needed --noconfirm "${missing[@]}" || ok=false
  else pkexec pacman -S --needed --noconfirm "${missing[@]}" || ok=false; fi
  if [[ ${ok:-true} != true ]]; then
    echo "error: package install failed. If the package database is stale, run omarchy-update (or: sudo pacman -Syu) and retry." >&2
    exit 1
  fi
fi

# Build outside the checkout: omarchy-shell watches ~/.config/omarchy/plugins and
# reloads every plugin (killing this script's parent) on each file written there.
# One build dir per checkout path, so two checkouts never share stale objects.
BUILD=$CACHE/build/$(printf %s "$HERE" | md5sum | cut -c1-10)
LOG=$CACHE/build.log
mkdir -p "$BUILD"
echo "› building daemon (log: $LOG)"
if ! make -C "$HERE/daemon" BUILD="$BUILD" -j"$(nproc)" >"$LOG" 2>&1; then
  tail -n 15 "$LOG" >&2
  echo "error: build failed; full log: $LOG" >&2
  exit 1
fi

echo "› installing to $LIB, $DATA"
install -d "$LIB" "$DATA/models" "$DATA/assets" "$BIN"
install -m 755 "$BUILD/camera-effects-server" "$LIB/camera-effects-server"
install -m 755 "$SETUP" "$LIB/camera-effects-setup"
install -m 644 "$HERE"/models/*.onnx "$DATA/models/"
install -m 644 "$HERE"/assets/*.png "$DATA/assets/"
ln -sfn "$LIB/camera-effects-server" "$BIN/camera-effects-server"
# Remember which checkout commit this build came from (the shell plugin rebuilds
# automatically after `omarchy plugin update` when this no longer matches).
git -C "$HERE" rev-parse HEAD > "$LIB/installed-commit" 2>/dev/null || date +%s > "$LIB/installed-commit"

echo "› installing shell plugin"
mkdir -p "$(dirname "$PLUGIN")"
# When this checkout *is* the plugin dir (omarchy plugin add), nothing to link.
if [[ $(readlink -f "$PLUGIN" 2>/dev/null) != "$(readlink -f "$HERE")" ]]; then
  if [[ -L $PLUGIN || ! -e $PLUGIN ]]; then
    ln -sfn "$HERE" "$PLUGIN"   # dev mode only; remove it before `omarchy plugin add`
    omarchy-shell -q shell rescanPlugins 2>/dev/null || true
  fi
fi
# Enable once: `enable … right` on a widget already on the bar moves it, so only ask
# while the shell does not list it as enabled (the rescan is async: retry briefly).
if command -v omarchy-shell >/dev/null && omarchy-shell -q shell ping >/dev/null 2>&1; then
  enabled() { omarchy-plugin-list 2>/dev/null | awk -v id="$ID" '$1 == id && $2 == "enabled" { found = 1 } END { exit !found }'; }
  for _ in 1 2 3 4 5; do
    if enabled; then break; fi
    if omarchy-plugin-enable "$ID" right >/dev/null 2>&1; then echo "  enabled $ID"; break; fi
    sleep 1
  done
  enabled || echo "  note: not enabled yet; run: omarchy plugin enable $ID"
else
  echo "  note: omarchy-shell is not running; enable later with: omarchy plugin enable $ID"
fi

hidden=false; ls /etc/udev/rules.d/71-camera-effects-hide-*.rules >/dev/null 2>&1 && hidden=true
if [[ $MODE == --no-root ]]; then
  echo "› skipping the root part; to create the device and refresh the root-owned copies, run:"
  echo "    sudo $LIB/camera-effects-setup install $LIB/camera-effects-server $LIB/camera-effects-setup"
  $hidden && echo "  (your cameras are hidden: until then the shell runs the previous privileged daemon copy)"
else
  # Creates the device (now + at boot) and refreshes the root-owned copies of the
  # setup script (what the shell pkexecs from then on) and, when cameras are
  # hidden, of the setgid daemon.
  echo "› creating the virtual camera and refreshing the root-owned copies (root)"
  privileged install "$LIB/camera-effects-server" "$LIB/camera-effects-setup"
fi

echo "done. The Camera icon appears in the bar (if not: omarchy-shell shell rescanPlugins, or omarchy-restart-shell)."
