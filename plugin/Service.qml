import QtQuick
import Quickshell
import Quickshell.Io

// Headless service: owns the irisd daemon (starts it, restarts it if it dies)
// and keeps a control connection to it. Everything the panel shows comes from
// the daemon's state pushes; everything the panel changes goes through set().
Item {
  id: root

  property var shell: null
  property var manifest: null

  // ---- daemon state (mirrors the JSON pushed by irisd) ----
  property var state: ({})
  readonly property bool connected: sockConnected
  readonly property bool running: !!state.running          // camera is being read + processed right now
  readonly property int consumers: state.consumers || 0     // apps holding the virtual camera open
  readonly property var settings: state.settings || ({})   // effective settings for the current camera
  readonly property bool sameForAll: state.sameForAll !== false
  readonly property var cameras: state.cameras || []
  readonly property var camera: state.camera || ({})
  readonly property string loopback: state.loopback || ""
  readonly property string loopbackLabel: state.loopbackLabel || "Iris Camera"
  readonly property string error: state.error || ""
  readonly property bool hideRaw: !!state.hideRaw
  readonly property bool block: !!state.block                // camera blocked: placeholder instead of the webcam (global)
  readonly property string blockSource: state.blockSource || ""  // "" = built-in card, else an image/video path
  readonly property int fps: state.fps || 0
  readonly property string gesture: state.gesture || ""
  readonly property var reactionNames: state.reactions || ["hearts", "thumbsup", "thumbsdown", "balloons", "confetti", "fireworks", "rain", "lasers"]
  readonly property bool deviceMissing: error.indexOf("virtual camera device missing") !== -1
  property string daemonLog: ""
  property int restarts: 0
  // Set when the daemon binary is present but cannot start (exit 127: a library
  // missing after a system update, a stale privileged copy); cleared once it talks to us.
  property string daemonError: ""

  readonly property string runtimeDir: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/iris"
  readonly property string socketPath: runtimeDir + "/ctl.sock"
  readonly property string homeDir: Quickshell.env("HOME") || ""
  readonly property string libDir: homeDir + "/.local/lib/iris"
  readonly property string setupScript: libDir + "/iris-setup"
  readonly property string daemonBinary: libDir + "/irisd"
  readonly property string privilegedBinary: "/usr/local/lib/iris/irisd"
  // Root-owned copy of the setup script (installed by `iris-setup install`).
  // Preferred for pkexec so that what runs as root is not user-writable.
  readonly property string privilegedSetupScript: "/usr/local/lib/iris/iris-setup"
  // The plugin checkout (this file lives in <repo>/plugin/).
  readonly property string repoDir: decodeURIComponent(String(Qt.resolvedUrl("..")).replace(/^file:\/\//, "").replace(/\/$/, ""))
  readonly property string cacheDir: (Quickshell.env("XDG_CACHE_HOME") || (homeDir + "/.cache")) + "/iris"
  readonly property string installLog: cacheDir + "/install.log"
  property bool installed: false   // daemon binary present in ~/.local/lib

  // ---- commands ----
  function send(obj) {
    if (!sockConnected) return false
    sock.write(JSON.stringify(obj) + "\n")
    sock.flush()
    return true
  }
  function set(patch) { return send({ cmd: "set", settings: patch }) }
  function setSetting(key, value) { var p = {}; p[key] = value; return set(p) }
  function setSameForAll(v) { return send({ cmd: "set", sameForAll: !!v }) }
  function selectCamera(busOrPath) { return send({ cmd: "set", camera: busOrPath }) }
  function react(name) { return send({ cmd: "react", name: name }) }
  function rescan() { return send({ cmd: "rescan" }) }
  function refresh() { return send({ cmd: "get" }) }

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
    // The root-owned script copy when it exists (after the first install), else ours.
    setupProc.command = ["sh", "-c", 'if [ -f "$1" ]; then s=$1; else s=$2; fi; shift 2; exec pkexec "$s" "$@"',
                         "iris-setup-run", privilegedSetupScript, setupScript].concat(args)
    setupProc.running = true
  }
  property bool setupBusy: setupProc.running || installProc.running
  property string setupOutput: ""

  Process {
    id: setupProc
    stdout: StdioCollector { onStreamFinished: root.setupOutput = String(text).trim() }
    stderr: StdioCollector { onStreamFinished: if (String(text).trim() !== "") root.setupOutput = String(text).trim() }
    onExited: function(code) {
      Qt.callLater(root.refresh)
      restartTimer.interval = 1000  // not whatever backoff the last crash/orphan path left behind
      restartTimer.restart()  // hide rules switch the daemon binary; a restart picks it up
    }
  }

  // First run from a plain `omarchy plugin add` (and "Rebuild daemon" later):
  // build + install the user half (no password; the build goes to ~/.cache so
  // nothing is written inside the plugin dir, which the shell watches), then
  // create the device / refresh the root-owned copies (password). The whole
  // output goes to ~/.cache/iris/install.log.
  function install() {
    if (installProc.running) return
    setupOutput = ""
    daemonError = ""
    installProc.command = ["sh", "-c",
      'mkdir -p "$(dirname "$2")"; cd "$1" && ./install.sh --no-root >"$2" 2>&1; rc=$?; tail -n 4 "$2"; exit $rc',
      "iris-install", repoDir, installLog]
    installProc.running = true
  }
  Process {
    id: installProc
    stdout: StdioCollector { onStreamFinished: root.setupOutput = String(text).trim() }
    onExited: function(code) {
      if (code === 0) { restartTimer.restart(); Qt.callLater(function() { root.runSetup("install") }) }
      else root.setupOutput = "install failed (" + code + "), see " + root.installLog + ":\n" + root.setupOutput
      probeProc.running = true   // re-check the binary rather than trusting the exit code
    }
  }
  // Is the daemon binary there? Answers the "exit 127" question: not installed
  // yet (wait for install()) or installed but not startable (back off, tell the user).
  property bool probeAfterExit: false
  Process {
    id: probeProc
    command: ["sh", "-c", 'test -x "$1"', "probe", root.daemonBinary]
    onExited: function(code) {
      root.installed = code === 0
      var afterExit = root.probeAfterExit
      root.probeAfterExit = false
      if (!root.installed || daemon.running) return
      if (afterExit) {
        root.restarts += 1
        root.daemonError = "daemon cannot start (exit 127: a library changed after an update, or a stale privileged copy) — rebuild it"
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
      'if ls /etc/udev/rules.d/71-iris-hide-*.rules >/dev/null 2>&1 && [ -x "$1" ]; then exec "$1" run; fi; ' +
      'if [ -x "$2" ]; then exec "$2" run; fi; exec irisd run',
      "irisd-launch", privilegedBinary, daemonBinary]
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

  // ---- control connection ----
  // Quickshell's Socket cannot recover from a refused connection (the inner
  // QLocalSocket never emits `disconnected`, so `connected = true` is a no-op
  // afterwards). Recreate the Socket object for every attempt instead.
  property var sock: null
  readonly property bool sockConnected: sock ? sock.connected === true : false

  Component {
    id: sockComp
    Socket {
      path: root.socketPath
      connected: true
      parser: SplitParser {
        onRead: function(line) {
          try {
            var msg = JSON.parse(line)
            if (msg && msg.type === "state") { root.state = msg; if (root.daemonError !== "") root.daemonError = "" }
          } catch (e) { /* reply we don't care about */ }
        }
      }
      onConnectionStateChanged: {
        root.sockConnectedChanged()
        if (!connected) {
          root.state = ({})
          if (root.orphanQuit) { root.orphanQuit = false; restartTimer.interval = 1000; restartTimer.restart() }
          reconnectTimer.restart()
        }
      }
      onError: function(err) { reconnectTimer.restart() }
    }
  }

  function connectSocket() {
    if (sock) { sock.destroy(); sock = null }
    sock = sockComp.createObject(root)
    sockConnectedChanged()
  }

  Timer {
    id: reconnectTimer
    interval: 800
    repeat: false
    onTriggered: if (!root.sockConnected) root.connectSocket()
  }
  Timer {
    interval: 3000
    running: !root.sockConnected
    repeat: true
    onTriggered: if (!root.sockConnected) root.connectSocket()
  }

  Component.onCompleted: {
    probeProc.running = true
    daemon.running = true
    connectSocket()
  }
  Component.onDestruction: {
    if (daemon.running) daemon.signal(15)
  }
}
