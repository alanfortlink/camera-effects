import QtQuick
import Quickshell
import Quickshell.Io

// Canonical wire protocol + state surface for the camera-effects daemon.
//
// One Client = one control connection to camera-effects-server. It owns the
// socket, the JSON split-line parser and the read-only mirror of the daemon's
// pushed state, plus the set of commands that change it. It never spawns or
// owns the daemon itself.
//
// Two consumers:
//   * Service.qml embeds one instance; the shell's serviceFor gives the panels
//     that surface (built-in bar).
//   * A bar widget (Panel.qml) instantiates one directly when no service is
//     reachable (a replacement/third-party bar, where bar.shell is null). The
//     daemon is multi-client: extra connections are expected.
//
// Snapshot *detection* lives here (a fresh lastSnapshot -> snapshotTaken /
// snapshotFailed); the *action* (clipboard copy + notification) belongs to the
// widget that has the user on screen, so the headless room does not double it.
Item {
  id: root

  // ---- shared paths (same daemon, same files the service manages) ----
  readonly property string runtimeDir: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/camera-effects"
  readonly property string socketPath: runtimeDir + "/ctl.sock"
  readonly property string previewPath: runtimeDir + "/preview.jpg"   // written by the daemon while previewWanted (see Preview.qml)
  readonly property string homeDir: Quickshell.env("HOME") || ""
  readonly property string libDir: homeDir + "/.local/lib/camera-effects"
  readonly property string setupScript: libDir + "/camera-effects-setup"
  readonly property string daemonBinary: libDir + "/camera-effects-server"
  readonly property string privilegedBinary: "/usr/local/lib/camera-effects/camera-effects-server"
  readonly property string privilegedSetupScript: "/usr/local/lib/camera-effects/camera-effects-setup"
  readonly property string micHideScript: libDir + "/camera-effects-mic-hide"
  readonly property string repoDir: decodeURIComponent(String(Qt.resolvedUrl("..")).replace(/^file:\/\//, "").replace(/\/$/, ""))
  readonly property string cacheDir: (Quickshell.env("XDG_CACHE_HOME") || (homeDir + "/.cache")) + "/camera-effects"
  readonly property string installLog: cacheDir + "/install.log"

  // ---- daemon state (mirrors the JSON pushed by camera-effects-server) ----
  property var state: ({})
  readonly property bool connected: sockConnected
  readonly property bool covered: !!state.covered      // frames arrive but they are black (lens cover?)
  readonly property bool starting: !!state.starting   // camera opening/reopening: the panel shows a busy indicator
  readonly property bool running: !!state.running          // camera is being read + processed right now
  readonly property int consumers: state.consumers || 0     // apps holding the virtual camera open
  readonly property var consumerApps: state.consumerApps || []   // their process names
  readonly property bool previewOn: !!state.previewOn       // some client (our panel) is watching the preview: pipeline runs, preview.jpg is written
  readonly property var settings: state.settings || ({})   // effective settings for the current camera
  readonly property bool sameForAll: state.sameForAll !== false
  readonly property var cameras: state.cameras || []
  readonly property var camera: state.camera || ({})
  readonly property string loopback: state.loopback || ""
  readonly property string loopbackLabel: state.loopbackLabel || "Camera Effects"
  readonly property string error: state.error || ""
  readonly property bool hideRaw: !!state.hideRaw
  readonly property bool previewMirror: state.previewMirror !== false   // panel-only self-view mirroring
  readonly property bool block: !!state.block                // camera blocked: placeholder instead of the webcam (global)
  readonly property string blockSource: state.blockSource || ""  // "" = built-in card, else an image/video path
  // Framing of the placeholder image/video (the camera's own zoom/pan/fit live in `settings`).
  readonly property real blockZoom: state.blockZoom !== undefined ? state.blockZoom : 1
  readonly property real blockPanX: state.blockPanX || 0
  readonly property real blockPanY: state.blockPanY || 0
  readonly property string blockFit: state.blockFit || "cover"
  // Pan room of what is shown now, per axis, as a fraction of the output size
  // (0 = nothing to pan): the preview's drag maps pixels through it.
  readonly property var panRange: state.panRange || [0, 0]
  readonly property int fps: state.fps || 0
  readonly property string gesture: state.gesture || ""
  readonly property var reactionNames: state.reactions || ["hearts", "thumbsup", "thumbsdown", "balloons", "confetti", "fireworks", "rain", "lasers"]
  readonly property bool deviceMissing: error.indexOf("virtual camera device missing") !== -1
  // ---- microphone effects (the daemon's "mic" object) ----
  readonly property var mic: state.mic || ({})
  readonly property var micSettings: mic.settings || ({})       // effective settings for the current microphone
  readonly property var micSources: mic.sources || []           // real microphones PipeWire knows about
  readonly property var micSource: mic.source || ({})           // the one being read now
  readonly property string micWanted: mic.wanted || ""          // the chosen one ("" = system default)
  readonly property string micLabel: mic.label || "Microphone Effects"
  readonly property string micStatus: mic.status || ""
  readonly property int micConsumers: mic.consumers || 0
  readonly property bool micCapturing: !!mic.capturing          // the real microphone is open
  readonly property bool micMuted: !!mic.muted
  readonly property bool micListen: !!mic.listen                // the remembered switch position
  readonly property bool micListening: !!mic.listening          // the playback stream is really running
  readonly property bool micHideAll: !!mic.hideAll              // every real microphone hidden from apps
  readonly property bool micSameForAll: mic.sameForAll !== false
  readonly property real micIn: mic.inLevel || 0                // 0..1 peak before the effects
  readonly property real micOut: mic.outLevel || 0              // and after them
  readonly property var micToneOptions: mic.tones || []
  readonly property var micVoiceOptions: mic.voices || []
  readonly property var micSpaceOptions: mic.spaces || []

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
  // Every effect of the current camera back to the built-in defaults (the
  // global block / placeholder / preview-mirror settings are not touched).
  function reset() { return send({ cmd: "reset" }) }
  // The daemon saves its next output frame as a PNG under ~/Pictures/Camera Effects
  // and reports it in the state (lastSnapshot): see noteSnapshot for what happens then.
  function snapshot() { return send({ cmd: "snapshot" }) }
  function refresh() { return send({ cmd: "get" }) }
  // The preview is per control connection (it ends when the connection does):
  // remember it so a reconnect asks again.
  property bool previewWanted: false
  function setPreview(on) { previewWanted = !!on; return send({ cmd: "preview", on: previewWanted }) }

  function setMic(patch) { return send({ cmd: "set", mic: patch }) }
  function setMicSetting(key, value) { var p = {}; p[key] = value; return setMic({ settings: p }) }
  function selectMic(nodeName) { return setMic({ source: nodeName }) }
  function setMicMuted(v) { return setMic({ muted: !!v }) }
  // Hear yourself: the daemon plays the processed microphone to the default
  // sink while a panel's Mic tab is open (micpreview), and remembers the
  // switch across restarts — so this only has to set it.
  function setMicListen(v) { return setMic({ listen: !!v }) }
  function setMicSameForAll(v) { return setMic({ sameForAll: !!v }) }
  function micReset() { return send({ cmd: "micreset" }) }
  // Like the camera preview: while a panel shows the level meter the daemon
  // holds the real microphone open even with no app using the virtual one.
  // One client = one connection, so each holder is tracked here; several
  // panels under the built-in bar share the service's single embedded client
  // and therefore share this count (closing one does not release the
  // microphone under the other), while replacement bars keep one per panel.
  property bool micPreviewWanted: false
  property var micPreviewHolders: ({})
  function setMicPreview(on, who) {
    var key = who === undefined ? "panel" : String(who)
    var h = micPreviewHolders
    if (on) h[key] = true; else delete h[key]
    micPreviewHolders = h
    var want = false
    for (var k in h) { want = true; break }
    if (want === micPreviewWanted) return true
    micPreviewWanted = want
    return send({ cmd: "micpreview", on: micPreviewWanted })
  }

  // ---- snapshots: detect here, act in the widget ----
  signal snapshotTaken(string path)      // a new one was saved (the panel flashes its preview, copies, notifies)
  signal snapshotFailed(string error)    // the daemon could not save one
  // A state whose lastSnapshot.time moved is a fresh one (a snapshot asked for
  // by us, the CLI or IPC alike). The first state after a (re)connect only
  // records the time: it may carry a snapshot from before we were listening.
  // snapSeen < 0 = not adopted yet.
  property real snapSeen: -1
  function noteSnapshot(msg) {
    var snap = msg.lastSnapshot
    var t = snap && snap.time ? snap.time : 0
    var fresh = snapSeen >= 0 && t !== snapSeen && !!snap
    snapSeen = t
    if (!fresh) return
    if (snap.error) { snapshotFailed(String(snap.error)); return }
    snapshotTaken(String(snap.path))
  }

  // ---- repair surface (the daemon's keeper is the service; these run against
  // the same shared files when a widget has to act without it) ----
  property bool installed: true   // the daemon binary is expected to be present
  property string daemonLog: ""
  property string daemonError: ""   // set by the service's lifecycle; here for the widget's fallback reads
  property bool setupBusy: setupProc.running || installProc.running || micHideProc.running
  property string setupOutput: ""
  property string busyText: ""

  // Privileged operations go through pkexec so the shell's polkit agent asks
  // for the password (see Service.qml for the full notes).
  function runSetup(what, a, b) {
    if (setupProc.running) return
    var args
    if (what === "install") args = ["install", daemonBinary, setupScript]
    else if (what === "hide-all") args = a ? ["hide-raw", "on", daemonBinary] : ["hide-raw", "off"]
    else if (what === "hide-camera") args = b ? ["hide-raw", "on", daemonBinary, String(a)] : ["hide-raw", "off", String(a)]
    else return
    setupOutput = ""
    busyText = what === "install" ? "Setting up the virtual camera…" : "Applying…"
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
      if (code === 0) root.setupOutput = ""
      else if (code === 126 || code === 127) root.setupOutput = "Cancelled: the system part was not changed."
      else root.setupOutput = errText !== "" ? errText : (outText !== "" ? outText : "setup failed (" + code + ")")
      outText = ""; errText = ""
      Qt.callLater(root.refresh)
    }
  }
  // Build + install the user half (no password): for a widget-bound repair.
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
      if (code === 0) root.setupOutput = ""
      else root.setupOutput = "Build failed (" + code + "). Log: " + root.installLog + "\n" + tailText
      tailText = ""
    }
  }
  // Hiding the real microphones is a WirePlumber script + config fragment in
  // the user's own config (no root, unlike the camera's udev rules).
  function runMicHide(what, a, b) {
    if (micHideProc.running) return
    if (!installed) { setupOutput = "camera-effects is still installing; try again in a moment."; return }
    var args
    if (what === "all") args = ["all", a ? "on" : "off"]
    else if (what === "mic") args = ["mic", String(a), b ? "on" : "off"]
    else return
    setupOutput = ""
    busyText = "Applying…"
    micHideProc.command = [micHideScript].concat(args)
    micHideProc.running = true
  }
  Process {
    id: micHideProc
    property string errText: ""
    stderr: StdioCollector { onStreamFinished: micHideProc.errText = String(text).trim() }
    onExited: function(code) {
      root.busyText = ""
      root.setupOutput = code === 0 ? "" : (errText !== "" ? errText : "could not change the microphone rules (" + code + ")")
      errText = ""
      Qt.callLater(root.refresh)
    }
  }

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
            if (msg && msg.type === "state") { root.state = msg; if (root.daemonError !== "") root.daemonError = ""; root.noteSnapshot(msg) }
          } catch (e) { /* reply we don't care about */ }
        }
      }
      onConnectionStateChanged: {
        root.sockConnectedChanged()
        if (connected && root.previewWanted) root.setPreview(true)
        if (connected && root.micPreviewWanted) root.setMicPreview(true)
        if (!connected) {
          root.state = ({})
          root.snapSeen = -1
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

  Component.onCompleted: connectSocket()
}