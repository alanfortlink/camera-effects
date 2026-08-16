#!/bin/bash
# Build and install the Omarchy Camera plugin for the current user.
#   ./install.sh            build daemon, install files, enable the shell plugin, set up the device (asks for password)
#   ./install.sh --no-root  everything except the privileged device setup
#   ./install.sh --uninstall
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=$HOME/.local/lib/omarchy-camera
DATA=$HOME/.local/share/omarchy-camera
BIN=$HOME/.local/bin
PLUGIN=$HOME/.config/omarchy/plugins/tank.camera
ID=tank.camera

privileged() {  # run the setup script as root, silently if sudo allows it
  if sudo -n true 2>/dev/null; then sudo "$LIB/omarchy-camera-setup" "$@"; else pkexec "$LIB/omarchy-camera-setup" "$@"; fi
}

if [[ ${1:-} == --uninstall ]]; then
  omarchy-plugin-disable "$ID" 2>/dev/null || true
  privileged uninstall || true
  rm -rf "$LIB" "$DATA" "$BIN/camfxd" "$PLUGIN"
  echo "uninstalled (config left in ~/.config/omarchy/camera.json)"
  exit 0
fi

echo "› building daemon"
make -C "$HERE/daemon" -j"$(nproc)" >/dev/null

echo "› installing to $LIB, $DATA"
install -d "$LIB" "$DATA/models" "$DATA/assets" "$BIN"
install -m 755 "$HERE/daemon/build/camfxd" "$LIB/camfxd"
install -m 755 "$HERE/scripts/omarchy-camera-setup" "$LIB/omarchy-camera-setup"
install -m 644 "$HERE"/models/*.onnx "$DATA/models/"
install -m 644 "$HERE"/assets/*.png "$DATA/assets/"
ln -sfn "$LIB/camfxd" "$BIN/camfxd"

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
  echo "› creating the virtual camera (root)"
  privileged install
fi
# A "hide raw camera" rule means the shell runs the root-owned setgid copy of the
# daemon; refresh it so it matches the build we just installed.
if ls /etc/udev/rules.d/71-omarchy-camera-hide-*.rules >/dev/null 2>&1; then
  echo "› refreshing the privileged daemon copy (root)"
  privileged hide-raw on "$LIB/camfxd" || true
fi

echo "done. The Camera icon appears in the bar (reload the shell with: omarchy-shell reload, or log out/in)."
