import QtQuick
import Quickshell
import Quickshell.Io

// Headless service: owns the camera-effects-server daemon (starts it, restarts
// it if it dies) and connects to it through an embedded Client. Everything the
// panel shows comes from the daemon's state pushes; everything the panel
// changes goes through set().
//
// Panels under the built-in bar reach this service via shell.serviceFor and
// read the aliased surface below — exactly the surface a panel under a
// replacement bar gets from its own Client instance. One shared connection
// here (the caller of setMicPreview with its own key), so closing one panel
// does not release the microphone under the other.
Item {
  id: root

  property var shell: null
  property var manifest: null

  // The single control connection the built-in bar's panels share: everything
  // panel-facing in this file is an alias or wrapper over it.
  Client { id: client }

  // ---- shared paths (single source: the Client's, Service and Client agree) ----
  readonly property alias runtimeDir: client.runtimeDir
  readonly property alias socketPath: client.socketPath
  readonly property alias previewPath: client.previewPath
  readonly property alias homeDir: client.homeDir
  readonly property alias libDir: client.libDir
  readonly property alias setupScript: client.setupScript
  readonly property alias daemonBinary: client.daemonBinary
  readonly property alias privilegedBinary: client.privilegedBinary
  readonly property alias privilegedSetupScript: client.privilegedSetupScript
  readonly property alias micHideScript: client.micHideScript
  readonly property alias repoDir: client.repoDir
  readonly property alias cacheDir: client.cacheDir
  readonly property alias installLog: client.installLog

  // ---- panel-facing state surface (aliases the embedded Client) ----
  property alias state: client.state
  readonly property alias connected: client.connected
  readonly property alias covered: client.covered      // frames arrive but they are black (lens cover?)
  readonly property alias starting: client.starting     // camera opening/reopening: the panel shows a busy indicator
  readonly property alias running: client.running       // camera is being read + processed right now
  readonly property alias consumers: client.consumers   // apps holding the virtual camera open
  readonly property alias consumerApps: client.consumerApps
  readonly property alias previewOn: client.previewOn   // some client (our panel) is watching the preview
  readonly property alias settings: client.settings     // effective settings for the current camera
  readonly property alias sameForAll: client.sameForAll
  readonly property alias cameras: client.cameras
  readonly property alias camera: client.camera
  readonly property alias loopback: client.loopback
  readonly property alias loopbackLabel: client.loopbackLabel
  readonly property alias error: client.error
  readonly property alias hideRaw: client.hideRaw
  readonly property alias previewMirror: client.previewMirror
  readonly property alias block: client.block            // camera blocked: placeholder instead of the webcam (global)
  readonly property alias blockSource: client.blockSource
  readonly property alias blockZoom: client.blockZoom
  readonly property alias blockPanX: client.blockPanX
  readonly property alias blockPanY: client.blockPanY
  readonly property alias blockFit: client.blockFit
  readonly property alias panRange: client.panRange
  readonly property alias fps: client.fps
  readonly property alias gesture: client.gesture
  readonly property alias reactionNames: client.reactionNames
  readonly property alias deviceMissing: client.deviceMissing
  // ---- microphone effects (the daemon's "mic" object) ----
  readonly property alias mic: client.mic
  readonly property alias micSettings: client.micSettings
  readonly property alias micSources: client.micSources
  readonly property alias micSource: client.micSource
  readonly property alias micWanted: client.micWanted
  readonly property alias micLabel: client.micLabel
  readonly property alias micStatus: client.micStatus
  readonly property alias micConsumers: client.micConsumers
  readonly property alias micCapturing: client.micCapturing
  readonly property alias micMuted: client.micMuted
  readonly property alias micListen: client.micListen
  readonly property alias micListening: client.micListening
  readonly property alias micHideAll: client.micHideAll
  readonly property alias micSameForAll: client.micSameForAll
  readonly property alias micIn: client.micIn
  readonly property alias micOut: client.micOut
  readonly property alias micToneOptions: client.micToneOptions
  readonly property alias micVoiceOptions: client.micVoiceOptions
  readonly property alias micSpaceOptions: client.micSpaceOptions

  // ---- commands (forwards to the shared connection) ----
  function send(obj) { return client.send(obj) }
  function set(patch) { return client.set(patch) }
  function setSetting(key, value) { return client.setSetting(key, value) }
  function setSameForAll(v) { return client.setSameForAll(v) }
  function selectCamera(busOrPath) { return client.selectCamera(busOrPath) }
  function react(name) { return client.react(name) }
  function rescan() { return client.rescan() }
  function reset() { return client.reset() }
  function snapshot() { return client.snapshot() }
  function refresh() { return client.refresh() }
  function setPreview(on) { return client.setPreview(on) }
  function setMic(patch) { return client.setMic(patch) }
  function setMicSetting(key, value) { return client.setMicSetting(key, value) }
  function selectMic(nodeName) { return client.selectMic(nodeName) }
  function setMicMuted(v) { return client.setMicMuted(v) }
  function setMicListen(v) { return client.setMicListen(v) }
  function setMicSameForAll(v) { return client.setMicSameForAll(v) }
  function micReset() { return client.micReset() }
  // One shared connection: the holder count lives on the embedded Client.
  function setMicPreview(on, who) { return client.setMicPreview(on, who) }
  // Hiding microphones is a WirePlumber script in the user's config (no root).
  function runMicHide(what, a, b) { return client.runMicHide(what, a, b) }

  // A fresh snapshot (asked for by us, the CLI or IPC alike) surfaces here so
  // the panel flashes its preview; the widget does the copy + notification.
  signal snapshotTaken(string path)
  signal snapshotFailed(string error)
  Connections {
    target: client
    function onSnapshotTaken(path) { root.snapshotTaken(path) }
    function onSnapshotFailed(error) { root.snapshotFailed(error) }
  }
  readonly property alias sockConnected: client.sockConnected

  // ---- daemon lifecycle (owned by this service, not by Client) ----
  property string daemonLog: ""
  property int restarts: 0
  // Set when the daemon binary is present but cannot start (exit 127: a library
  // missing after a system update, a stale privileged copy); cleared once it talks to us.
  property string daemonError: ""
  property bool installed: false   // daemon binary present in ~/.local/lib
  property bool setupBusy: setupProc.running || installProc.running
  // setupOutput carries only what the user must act on (a failure, a cancelled
  // prompt). Progress lives in busyText and disappears on its own.
  property string setupOutput: ""
  property string busyText: ""

  // Privileged operations go through pkexec so the shell's polkit agent asks
  // for the password.
  //   runSetup("install")                    create the virtual camera (now + boot)
  //   runSetup("hide-all", true|false)       hide/unhide every physical camera
  //   runSetup("hide-camera", key, on)       hide/unhide one USB camera
  function runSetup(what, a, b) {
    if (setupProc.running) return
    var args
    // install: also refreshes the root-owned script copy and, when cameras are hidden, the setgid daemon copy
    if (what === "install") args = ["install", daemonBinary, setupScript]
    else if (what === "hide-all") args = a ? ["hide-raw", "on", daemonBinary] : ["hide-raw", "off"]
    else if (what === "hide-camera") args = b ? ["hide-raw", "on", daemonBinary, String(a)] : ["hide-raw", "off", String(a)]
    else return
    setupOutput = ""
    busyText = what === "install" ? "Setting up the virtual camera…" : "Applying…"
    // The root-owned script copy when it exists (after the first install), else ours.
    setupProc.command = ["sh", "-c", 'if [ -f "$1" ]; then s=$1; else s=$2; fi; shift 2; exec pkexec "$s" "$@"',
                         "camera-effects-setup-run", privilegedSetupScript, setupScript].concat(args)
    setupProc.running = true
  }
  Process {
    id: setupProc
    property string outText: ""
    property string errText: ""
    stdout: StdioCollector { onStreamFinished: setupProc.outText = String(text).trim() }
    stderr: StdioCollector { onStreamFinished: setupProc.errText = String(text).trim() }
    onExited: function(code) {
      root.busyText = ""
      // 126/127 = the polkit dialog was dismissed or pkexec is missing.
      if (code === 0) root.setupOutput = ""
      else if (code === 126 || code === 127) root.setupOutput = "Cancelled: the system part was not changed."
      else root.setupOutput = errText !== "" ? errText : (outText !== "" ? outText : "setup failed (" + code + ")")
      outText = ""; errText = ""
      Qt.callLater(root.refresh)
      restartTimer.interval = 1000  // not whatever backoff the last crash/orphan path left behind
      restartTimer.restart()  // hide rules switch the daemon binary; a restart picks it up
    }
  }

  // First run from a plain `omarchy plugin add` (and "Rebuild daemon" later):
  // build + install the user half (no password; the build goes to ~/.cache so
  // nothing is written inside the plugin dir, which the shell watches), then
  // create the device / refresh the root-owned copies (password). The whole
  // output goes to ~/.cache/camera-effects/install.log.
  function install() {
    if (installProc.running) return
    setupOutput = ""
    if (busyText === "") busyText = "Building the daemon…"
    daemonError = ""
    installProc.command = ["sh", "-c",
      'mkdir -p "$(dirname "$2")"; cd "$1" && ./install.sh --no-root >"$2" 2>&1; rc=$?; tail -n 4 "$2"; exit $rc',
      "camera-effects-install", repoDir, installLog]
    installProc.running = true
  }
  Process {
    id: installProc
    property string tailText: ""
    stdout: StdioCollector { onStreamFinished: installProc.tailText = String(text).trim() }
    onExited: function(code) {
      root.busyText = ""
      // No restart here: while cameras are hidden the daemon runs the privileged
      // copy, which this build has not refreshed yet. Restarting now would launch
      // the stale one again and report the same failure. rootCheckProc restarts
      // once the root half is current (or was not needed).
      if (code === 0) { root.setupOutput = ""; rootCheckProc.running = true }
      else root.setupOutput = "Build failed (" + code + "). Log: " + root.installLog + "\n" + tailText
      tailText = ""
      probeProc.running = true   // re-check the binary rather than trusting the exit code
    }
  }
  // After a build: only ask for the password when the root-owned copies (setup
  // script; the setgid daemon when cameras are hidden) or the device are out of
  // date. Exit 1 = needs root, 0 = all current.
  Process {
    id: rootCheckProc
    command: ["sh", "-c",
      'lib=$1; root=/usr/local/lib/camera-effects; ' +
      '[ -x "$root/camera-effects-setup" ] || exit 1; cmp -s "$lib/camera-effects-setup" "$root/camera-effects-setup" || exit 1; ' +
      'if ls /etc/udev/rules.d/71-camera-effects-hide-*.rules >/dev/null 2>&1; then [ -r "$root/camera-effects-server" ] || exit 1; cmp -s "$lib/camera-effects-server" "$root/camera-effects-server" || exit 1; fi; ' +
      'exit 0',
      "camera-effects-rootcheck", root.libDir]
    onExited: function(code) {
      if (code !== 0 || root.deviceMissing) { root.runSetup("install"); return }   // setupProc restarts the daemon
      root.busyText = ""
      restartTimer.interval = 1000
      restartTimer.restart()
    }
  }
  // Auto-update: after `omarchy plugin update` the shell reloads this plugin, so
  // on start we compare the checkout with what install.sh last installed and
  // rebuild silently when they differ (a password is asked only if root copies
  // are stale, see rootCheckProc).
  Process {
    id: updateCheckProc
    command: ["sh", "-c",
      'a=$(git -C "$1" rev-parse HEAD 2>/dev/null) || exit 0; b=$(cat "$2/installed-commit" 2>/dev/null); ' +
      '[ -x "$2/camera-effects-server" ] || exit 0; [ -n "$a" ] && [ "$a" != "$b" ] && exit 3; exit 0',
      "camera-effects-updatecheck", root.repoDir, root.libDir]
    onExited: function(code) { if (code === 3) { root.busyText = "Updating…"; root.install() } }
  }
  // Is the daemon binary there? Answers the "exit 127" question: not installed
  // yet (wait for install()) or installed but not startable (back off, tell the user).
  property bool probeAfterExit: false
  // A system update that moves a library the daemon links against (onnxruntime
  // renames its symbol version every release, opencv bumps its soname) leaves an
  // installed binary that no longer loads. Rebuilding is the fix and needs no
  // password, so do it ourselves — but only once per session, or a build that
  // does not fix it would loop.
  property bool rebuiltForLoadError: false
  Process {
    id: probeProc
    // 1 = no build installed, 2 = the privileged copy the launcher prefers is not
    // the one we built (only refreshing it as root helps), 0 = ours is what runs.
    command: ["sh", "-c",
      'test -x "$1" || exit 1; ' +
      'ls /etc/udev/rules.d/71-camera-effects-hide-*.rules >/dev/null 2>&1 || exit 0; ' +
      '[ -r "$2" ] || exit 0; cmp -s "$1" "$2" || exit 2; exit 0',
      "probe", root.daemonBinary, root.privilegedBinary]
    onExited: function(code) {
      root.installed = code !== 1
      var afterExit = root.probeAfterExit
      root.probeAfterExit = false
      if (!root.installed || daemon.running) return
      if (afterExit) {
        // A system update moves a library out from under the installed binary
        // (onnxruntime renames its symbol version every release, opencv bumps
        // its soname) and it stops loading. Fix it without making the user work
        // out which half is stale — but only once per session, so a repair that
        // does not help falls through to the message instead of looping.
        if (!root.rebuiltForLoadError) {
          root.rebuiltForLoadError = true
          if (code === 2) { root.runSetup("install"); return }
          root.busyText = "Rebuilding after a system update…"
          root.install()
          return
        }
        root.restarts += 1
        root.daemonError = code === 2
          ? "daemon cannot start: the privileged copy is out of date — approve the password prompt, or run: sudo " + root.privilegedSetupScript + " install " + root.daemonBinary + " " + root.setupScript
          : "daemon cannot start (exit 127: a library changed after an update, or a stale privileged copy) — rebuild it"
        restartTimer.interval = Math.min(10000, 1000 + root.restarts * 1000)
      }
      restartTimer.restart()
    }
  }
  // While not installed, keep looking: an install interrupted by a shell reload
  // (or run from a terminal) finishes on its own and we pick the binary up.
  Timer {
    interval: 5000
    repeat: true
    running: !root.installed && !installProc.running && !probeProc.running
    onTriggered: probeProc.running = true
  }

  // ---- daemon lifecycle ----
  // Prefer the privileged (setgid camerad) copy while any camera is hidden,
  // otherwise the user's own build.
  function daemonCommand() {
    return ["sh", "-c",
      'if ls /etc/udev/rules.d/71-camera-effects-hide-*.rules >/dev/null 2>&1 && [ -x "$1" ]; then exec "$1" run; fi; ' +
      'if [ -x "$2" ]; then exec "$2" run; fi; exec camera-effects-server run',
      "camera-effects-launch", privilegedBinary, daemonBinary]
  }

  Process {
    id: daemon
    command: root.daemonCommand()
    running: false
    stderr: SplitParser {
      onRead: function(line) {
        var l = root.daemonLog + line + "\n"
        if (l.length > 4000) l = l.slice(l.length - 4000)
        root.daemonLog = l
      }
    }
    onExited: function(code, status) {
      root.state = ({})
      if (code === 127) { root.probeAfterExit = true; probeProc.running = true; return }  // not installed yet, or unloadable: the probe decides
      root.restarts += 1
      if (code !== 3 && root.restarts >= 3) root.daemonError = "daemon keeps exiting (code " + code + ") — rebuild it or check the log"
      restartTimer.interval = code === 3 ? 5000 : Math.min(10000, 1000 + root.restarts * 1000)  // 3 = another instance holds the lock
      restartTimer.restart()
    }
  }

  // Set while we wait for an orphaned daemon to honour our quit; the socket
  // dropping is the signal to start our own without the 5 s wait.
  property bool orphanQuit: false
  Timer {
    id: restartTimer
    interval: 1000
    repeat: false
    onTriggered: {
      if (daemon.running) { daemon.signal(15); return }   // restart requested (setup changed things)
      // Not our process but something answers on the socket: a daemon orphaned by
      // a previous shell. Ask it to quit so we can start (and later restart) our own.
      if (root.sockConnected) { root.send({ cmd: "quit" }); root.orphanQuit = true; interval = 5000; restart(); return }
      daemon.command = root.daemonCommand()
      daemon.running = true
    }
  }

  // Reset the backoff once the daemon has been alive for a while.
  Timer { interval: 60000; running: daemon.running; repeat: false; onTriggered: root.restarts = 0 }

  Component.onCompleted: {
    probeProc.running = true
    daemon.running = true
    updateCheckProc.running = true
  }
  Component.onDestruction: {
    if (daemon.running) daemon.signal(15)
  }
}