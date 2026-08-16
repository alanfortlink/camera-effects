#!/bin/bash
# Build and install the Iris Camera plugin for the current user.
#   ./install.sh            build daemon, install files, enable the shell plugin, set up the device (asks for password)
#   ./install.sh --no-root  everything except the privileged device setup
#   ./install.sh --uninstall
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=$HOME/.local/lib/iris
DATA=$HOME/.local/share/iris
BIN=$HOME/.local/bin
PLUGIN=$HOME/.config/omarchy/plugins/alanfortlink.iris
ID=alanfortlink.iris

privileged() {  # run the setup script as root, silently if sudo allows it
  if sudo -n true 2>/dev/null; then sudo "$LIB/iris-setup" "$@"; else pkexec "$LIB/iris-setup" "$@"; fi
}

if [[ ${1:-} == --uninstall ]]; then
  omarchy-plugin-disable "$ID" 2>/dev/null || true
  privileged uninstall || true
  rm -rf "$LIB" "$DATA" "$BIN/irisd"
  [[ -L $PLUGIN ]] && rm -f "$PLUGIN"   # dev symlink; a real checkout is removed with `omarchy plugin remove alanfortlink.iris`
  echo "uninstalled (config left in ~/.config/iris/config.json)"
  exit 0
fi

# Build/runtime dependencies (all in the Omarchy/Arch repos).
missing=()
for p in gcc make pkgconf opencv onnxruntime pipewire qt6-multimedia v4l2loopback-dkms; do
  pacman -Q "$p" >/dev/null 2>&1 || missing+=("$p")
done
if ((${#missing[@]})); then
  echo "› installing missing packages: ${missing[*]}"
  if sudo -n true 2>/dev/null; then sudo pacman -S --needed --noconfirm "${missing[@]}"
  else pkexec pacman -S --needed --noconfirm "${missing[@]}"; fi
fi

echo "› building daemon"
make -C "$HERE/daemon" -j"$(nproc)" >/dev/null

echo "› installing to $LIB, $DATA"
install -d "$LIB" "$DATA/models" "$DATA/assets" "$BIN"
install -m 755 "$HERE/daemon/build/irisd" "$LIB/irisd"
install -m 755 "$HERE/scripts/iris-setup" "$LIB/iris-setup"
install -m 644 "$HERE"/models/*.onnx "$DATA/models/"
install -m 644 "$HERE"/assets/*.png "$DATA/assets/"
ln -sfn "$LIB/irisd" "$BIN/irisd"

echo "› installing shell plugin"
mkdir -p "$(dirname "$PLUGIN")"
# When this checkout *is* the plugin dir (omarchy plugin add), nothing to link.
if [[ $(readlink -f "$PLUGIN" 2>/dev/null) != "$(readlink -f "$HERE")" ]]; then
  if [[ -L $PLUGIN || ! -e $PLUGIN ]]; then ln -sfn "$HERE" "$PLUGIN"; fi
fi
if command -v omarchy-plugin-enable >/dev/null; then
  omarchy-plugin-enable "$ID" right || true
fi

if [[ ${1:-} != --no-root ]]; then
  # Creates the device (now + at boot) and installs the root-owned copy of the
  # setup script that the shell pkexecs from then on.
  echo "› creating the virtual camera (root)"
  privileged install
fi
# A "hide raw camera" rule means the shell runs the root-owned setgid copy of the
# daemon; refresh it so it matches the build we just installed (rules untouched).
if ls /etc/udev/rules.d/71-iris-hide-*.rules >/dev/null 2>&1; then
  if [[ ${1:-} != --no-root ]] || sudo -n true 2>/dev/null; then
    echo "› refreshing the privileged daemon copy (root)"
    privileged refresh-daemon "$LIB/irisd" || true
  else
    echo "› note: cameras are hidden, so the shell runs the privileged daemon copy; run: sudo $LIB/iris-setup refresh-daemon $LIB/irisd"
  fi
fi

echo "done. The Camera icon appears in the bar (reload the shell with: omarchy-shell reload, or log out/in)."
