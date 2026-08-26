import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Bar icon + popup panel for Camera Effects (the macOS "Video Effects" menu,
// Omarchy style). Five tabs: Video (camera picker, framing), Look (portrait,
// light, background, filter), Fun (face effects, ambience, reactions), Mic
// (microphone picker, meters, listen, volume, presets and effects) and
// Privacy (block the camera, hide the raw devices from apps). Above them sits
// the live preview — drag to pan, wheel to zoom — which the Mic tab replaces
// with its level meters. All state lives in the camera-effects-server daemon
// (see Service.qml); this file only renders and forwards.
Panel {
  id: root
  moduleName: "alanfortlink.camera-effects"
  ipcTarget: "alanfortlink.camera-effects"
  // manageIpc: false so this panel can own the single IpcHandler the target
  // permits — needed for `snap` below.
  manageIpc: false

  // The shell mounts our service at startup (kinds: service); calling
  // ensureService() from a binding would mutate the registry it reads (loop).
  // The panel closes only when the user says so (bar icon, Esc, IPC) or when the
  // bar hands the popout to another widget — not on a stray click outside, and
  // never while a file/colour chooser is up (that click lands on the dialog).
  function close() {
    if (!allowClose) return
    allowClose = false
    controller.hide()
  }
  function userClose() { allowClose = true; close() }
  function toggle() { if (opened) userClose(); else open() }
  function closeForPopoutSwitch() { allowClose = true; popoutSwitchClosing = true; controller.hide(); Qt.callLater(function() { popoutSwitchClosing = false }) }
  property bool allowClose: false
  property bool resetConfirm: false
  property bool micResetConfirm: false
  // The daemon wants frames but the camera is still opening (a USB reopen after
  // a Center Stage / capture-size change costs a second or two).
  readonly property bool busy: !!svc && svc.starting

  readonly property var svc: bar && bar.shell ? bar.shell.serviceFor("alanfortlink.camera-effects") : null
  readonly property bool inUse: svc ? svc.running : false
  readonly property bool connected: svc ? svc.connected : false
  readonly property var s: svc ? svc.settings : ({})
  readonly property var m: svc ? svc.micSettings : ({})     // microphone effects
  readonly property var cams: svc ? svc.cameras : []
  readonly property bool multiCam: cams.length > 1
  readonly property bool alwaysShow: setting("alwaysShow", true)
  readonly property bool blocked: svc ? svc.block : false

  // Framing (zoom / pan / fit) edits the camera's settings, or the
  // placeholder's block* settings while the camera is blocked: always what
  // the preview shows. The built-in card is not framed; with Center Stage on
  // the pan is automatic and the zoom is its minimum.
  readonly property real zoomNow: blocked ? (svc ? svc.blockZoom : 1) : (s.zoom !== undefined ? s.zoom : 1)
  readonly property real panXNow: blocked ? (svc ? svc.blockPanX : 0) : (s.panX || 0)
  readonly property real panYNow: blocked ? (svc ? svc.blockPanY : 0) : (s.panY || 0)
  readonly property string fitNow: blocked ? (svc ? svc.blockFit : "cover") : (s.fit || "cover")
  readonly property bool centerStageOn: !blocked && !!s.centerStage
  readonly property bool framingEnabled: !!svc && (!blocked || svc.blockSource !== "")
  readonly property bool panEnabled: framingEnabled && !centerStageOn
  function setFraming(patch) {  // keys zoom / panX / panY / fit, sent as block* while blocked
    if (!svc) return
    var p = {}
    for (var k in patch) p[blocked ? "block" + k.charAt(0).toUpperCase() + k.slice(1) : k] = patch[k]
    svc.set(p)
  }
  // The zoom as last sent by the wheel: the daemon echoes settings ~150 ms
  // later, so a quick scroll must not step from a stale value.
  property real zoomLive: 1
  onZoomNowChanged: if (!zoomTouched.running) zoomLive = zoomNow
  Timer { id: zoomTouched; interval: 400; onTriggered: root.zoomLive = root.zoomNow }
  Component.onCompleted: { zoomLive = zoomNow; activePreset = matchPreset() }

  // The preview is served by the daemon (a JPEG it writes while we ask for
  // it, see Preview.qml), not read from the virtual camera: consumers are
  // all real apps. While the panel is open the daemon runs the pipeline for
  // the preview even with no app watching (svc.previewOn).
  readonly property bool previewActive: previewLoader.active
  onPreviewActiveChanged: if (svc) svc.setPreview(previewActive)
  Component.onDestruction: {
    if (svc && previewActive) svc.setPreview(false)
    if (svc && micPreviewActive) svc.setMicPreview(false, root)
  }
  readonly property int appCount: svc ? svc.consumers : 0
  readonly property bool appsConnected: appCount > 0
  readonly property bool previewOnly: !!svc && svc.previewOn && !appsConnected
  // The microphone is only opened for the level meter while the Mic tab is on
  // screen — the audio counterpart of previewActive.
  readonly property bool micPreviewActive: opened && tab === "mic" && !!svc
  // Opening the tab starts playing the microphone back if that is how the
  // switch was left last time (the daemon remembers it and ties the playback
  // to this same flag), so nothing is left echoing once the panel is closed.
  onMicPreviewActiveChanged: if (svc) svc.setMicPreview(micPreviewActive, root)

  readonly property color fg: bar ? bar.foreground : Color.foreground
  readonly property color dim: Qt.darker(fg, 1.45)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property int rowH: Style.spacing.controlHeight
  // ToggleSwitch reserves `cursorPad` around its track for the hover ring, so
  // its visible edge sits this far inside the row. Every other trailing
  // control (dropdown, slider, field) gets the same inset so they line up.
  readonly property int trailInset: Style.space(6)
  property bool pickerMissing: false
  // The binary is there but nothing answered for a while: the daemon is stuck or
  // unloadable (e.g. a library changed after a system update). Offer a rebuild.
  property bool stalled: false
  Timer {
    interval: 10000
    running: !!root.svc && root.svc.installed && !root.connected && !root.svc.setupBusy
    onTriggered: root.stalled = true
  }
  onConnectedChanged: if (connected) stalled = false
  readonly property bool needsRebuild: !!svc && svc.installed && !connected && (svc.daemonError !== "" || stalled)

  // The daemon does not say when a reaction is playing (a `reactionsActive`
  // field in its state would make this exact), so approximate: hide the
  // gesture badge for the length of an animation after a reaction button is
  // clicked, or once a shown gesture has been held long enough to fire
  // (~0.6 s steady in the daemon).
  Timer { id: badgeHide; interval: 3500 }
  Timer { id: gestureFire; interval: 600; onTriggered: badgeHide.restart() }
  Connections {
    target: root.svc
    function onGestureChanged() {
      if (root.svc && root.svc.gesture !== "" && !!root.s.reactions) gestureFire.restart()
      else gestureFire.stop()
    }
    function onSnapshotTaken(path) { snapFlash.restart() }   // shutter feedback (also for a CLI/IPC snapshot while open)
  }

  readonly property bool micMuted: !!svc && svc.micMuted
  visible: alwaysShow || inUse || micMuted
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  // Snapshot: 3 · 2 · 1 over the preview, then the daemon saves its next
  // output frame (what apps see) as a PNG; the service copies it to the
  // clipboard and notifies, the preview flashes white here.
  property int snapCount: 0   // 3..1 while counting down, 0 otherwise
  function snap() {
    if (!svc || snapCount > 0) return
    if (!opened) open()   // the countdown is drawn over the preview
    snapCount = 3
    snapTimer.restart()
  }
  Timer {
    id: snapTimer
    interval: 1000
    repeat: true
    onTriggered: {
      root.snapCount--
      if (root.snapCount > 0) return
      snapTimer.stop()
      if (root.svc) root.svc.snapshot()
    }
  }
  IpcHandler {
    target: root.ipcTarget
    function open() { root.open() }
    function close() { root.userClose() }
    function show() { root.open() }
    function hide() { root.close() }
    function toggle() { root.toggle() }
    function snap() { root.snap() }   // omarchy-shell alanfortlink.camera-effects snap
    // Mute without opening anything — the point of a virtual microphone is that
    // one switch covers every app, so it deserves a key binding:
    //   bind = , code:121, exec, omarchy-shell alanfortlink.camera-effects micMute
    function micMute() { if (root.svc) root.svc.setMicMuted(!root.svc.micMuted) }
    function micMuteOn() { if (root.svc) root.svc.setMicMuted(true) }
    function micMuteOff() { if (root.svc) root.svc.setMicMuted(false) }
  }

  function basename(p) {
    var str = String(p || "")
    return str.slice(str.lastIndexOf("/") + 1)
  }
  // "File: foo.y4m" (the daemon's label for a file source) -> "foo.y4m".
  function camName(c) {
    if (!c || !c.name) return "no camera"
    var n = String(c.name)
    return n.indexOf("File: ") === 0 ? n.slice(6) : n
  }
  function subtitle() {
    if (!svc) return "Service not loaded"
    if (svc.setupBusy) return "Working…"
    if (!svc.installed) return "Not installed"
    if (svc.daemonError && !connected) return svc.daemonError
    if (stalled && !connected) return "Daemon not responding"
    if (!connected) return "Starting…"
    if (svc.deviceMissing) return "Needs setup"
    if (svc.error) return svc.error
    if (blocked) return "BLOCKED" + (appsConnected ? " · " + appCount + (appCount > 1 ? " apps" : " app") : "")
    var cam = camName(svc.camera)
    var fps = inUse && svc.fps ? " · " + svc.fps + " FPS" : ""
    if (appsConnected) return "In use · " + cam + fps
    if (previewOnly && inUse) return "Preview · " + cam + fps
    return "Idle · " + cam
  }
  function glyph(name) {
    return ({ hearts: "❤️", thumbsup: "👍", thumbsdown: "👎", balloons: "🎈", confetti: "🎊", fireworks: "🎆", rain: "🌧️", lasers: "🔦" })[name] || name
  }
  function effectTitle(name) {
    return ({ hearts: "Hearts", thumbsup: "Thumbs up", thumbsdown: "Thumbs down", balloons: "Balloons",
              confetti: "Confetti", fireworks: "Fireworks", rain: "Rain", lasers: "Lasers" })[name] || name
  }
  // The hand gesture that triggers each reaction (what the daemon "sees").
  function gestureTitle(name) {
    return ({ hearts: "🫶 Heart hands", thumbsup: "👍 Thumbs up", thumbsdown: "👎 Thumbs down", balloons: "✌️ Peace sign",
              confetti: "✌️✌️ Two peace signs", fireworks: "👍👍 Two thumbs up", rain: "👎👎 Two thumbs down", lasers: "🤘🤘 Two rock signs" })[name] || name
  }
  function hint(name) { return effectTitle(name) + " · " + gestureTitle(name) }
  function shortName(c) {
    var n = camName(c)
    return n.length > 26 ? n.slice(0, 25) + "…" : n
  }
  function cameraOptions(list, cur) {
    var opts = list.map(function(c) { return { value: String(c.bus), label: camName(c) } })
    if (cur && cur.bus && !list.some(function(c) { return String(c.bus) === String(cur.bus) }))
      opts.push({ value: String(cur.bus), label: camName(cur) })
    return opts
  }
  // The daemon pushes state every ~150 ms while running; a fresh array per push
  // would rebuild the Dropdown/Repeater delegates each time (a hide switch could
  // vanish mid-click). Derive the option lists from the JSON text of the inputs
  // instead, so they only change when the cameras/reactions themselves do.
  readonly property string camsJson: JSON.stringify([cams, svc && svc.camera ? svc.camera : null])
  readonly property string reactionsJson: JSON.stringify(svc ? svc.reactionNames : [])
  readonly property var cameraOpts: { var j = JSON.parse(camsJson); return cameraOptions(j[0], j[1]) }
  readonly property var hideableCams: JSON.parse(camsJson)[0].filter(function(c) { return !!c.key })
  readonly property var reactionList: JSON.parse(reactionsJson)
  // Same trick as camsJson: derive the microphone lists from the JSON text so
  // the delegates are not rebuilt on every state push.
  readonly property string micsJson: JSON.stringify([svc ? svc.micSources : [], svc ? svc.micWanted : "",
                                                     svc && svc.micSource ? svc.micSource : null])
  readonly property var micList: JSON.parse(micsJson)[0]
  readonly property bool multiMic: micList.length > 1
  readonly property var micOpts: {
    var j = JSON.parse(micsJson), list = j[0], cur = String(j[1] || "")
    var opts = [ { value: "", label: "Default microphone" } ]
    for (var i = 0; i < list.length; i++) opts.push({ value: String(list[i].name), label: micName(list[i]) })
    if (cur !== "" && !list.some(function(c) { return String(c.name) === cur })) opts.push({ value: cur, label: cur + " (not here)" })
    return opts
  }
  function micName(c) {
    if (!c) return "microphone"
    var n = String(c.description || c.name || "microphone")
    return n.length > 30 ? n.slice(0, 29) + "…" : n
  }
  readonly property var toneOpts: labelledOpts(svc ? svc.micToneOptions : [],
    { none: "None", warm: "Warm", bright: "Bright", clarity: "Clarity", podcast: "Podcast", telephone: "Telephone" })
  readonly property var voiceOpts: labelledOpts(svc ? svc.micVoiceOptions : [],
    { none: "None", deep: "Deep", chipmunk: "Chipmunk", robot: "Robot", alien: "Alien", megaphone: "Megaphone", monster: "Monster" })
  readonly property var spaceOpts: labelledOpts(svc ? svc.micSpaceOptions : [],
    { none: "None", room: "Room", hall: "Hall", cathedral: "Cathedral", echo: "Echo", underwater: "Underwater" })
  // The daemon owns the list of valid values; this only prettifies the names
  // (and falls back to the key for one it does not know yet).
  function labelledOpts(names, labels) {
    var list = names && names.length ? names : Object.keys(labels)
    return list.map(function(n) { return { value: String(n), label: labels[n] || String(n) } })
  }
  // ---- microphone presets -------------------------------------------------
  // A bundle of settings behind one chip. They cover the whole effect set (a
  // preset that left a robot voice on would be a lie), so picking one is a
  // complete answer and "Custom" simply means "none of these matches".
  readonly property var intensityOwner: ({ voiceIsolationIntensity: "voiceIsolation", noiseGateIntensity: "noiseGate",
                                           autoLevelIntensity: "autoLevel", deEsserIntensity: "deEsser" })
  readonly property var presetList: [
    { key: "clean", label: "Clean", set: { voiceIsolation: false, noiseGate: false, autoLevel: false, deEsser: false, humFilter: false,
                                           highPass: true, tone: "none", voice: "none", space: "none" } },
    { key: "voice", label: "Voice", set: { voiceIsolation: true, voiceIsolationIntensity: 0.6, noiseGate: false, autoLevel: false,
                                           deEsser: false, humFilter: false, highPass: true, tone: "none", voice: "none", space: "none" } },
    { key: "meeting", label: "Meeting", set: { voiceIsolation: true, voiceIsolationIntensity: 0.7, noiseGate: true, noiseGateIntensity: 0.55,
                                               autoLevel: true, autoLevelIntensity: 0.6, deEsser: true, deEsserIntensity: 0.5,
                                               humFilter: false, highPass: true, tone: "clarity", voice: "none", space: "none" } },
    { key: "podcast", label: "Podcast", set: { voiceIsolation: true, voiceIsolationIntensity: 0.4, noiseGate: false, autoLevel: true,
                                               autoLevelIntensity: 0.5, deEsser: true, deEsserIntensity: 0.6, humFilter: false,
                                               highPass: true, tone: "podcast", voice: "none", space: "none" } },
  ]
  // Which preset the current settings *are*. Derived, not stored — so editing
  // one switch lands on "Custom" by itself. Recomputed only when the settings
  // actually change (state arrives several times a second).
  readonly property string micSettingsKey: svc ? JSON.stringify(svc.micSettings) : ""
  property string activePreset: "custom"
  onMicSettingsKeyChanged: activePreset = matchPreset()
  function matchPreset() {
    var m = svc ? svc.micSettings : null
    if (!m) return "custom"
    for (var i = 0; i < presetList.length; i++) {
      var p = presetList[i]
      if (!p.set) continue
      var hit = true
      for (var k in p.set) {
        var a = m[k], b = p.set[k]
        // An intensity only counts while the effect it belongs to is on, so
        // nudging a Strength slider does not silently drop you to "Custom"
        // for a setting that is not even running.
        if (typeof b === "number") {
          var owner = intensityOwner[k]
          if (owner && !m[owner]) continue
          if (Math.abs((a === undefined ? -1 : a) - b) > 0.001) { hit = false; break }
          continue
        }
        if ((a === undefined ? false : a) !== b) { hit = false; break }
      }
      if (hit) return p.key
    }
    return "custom"
  }
  function applyPreset(key) {
    if (!svc) return
    for (var i = 0; i < presetList.length; i++) {
      if (presetList[i].key !== key || !presetList[i].set) continue
      var patch = {}
      for (var k in presetList[i].set) patch[k] = presetList[i].set[k]
      patch.enabled = true   // picking a preset is asking for effects
      svc.setMic({ settings: patch })
      return
    }
  }
  // What is on, in as few words as the header can hold.
  function labelFor(opts, v) {
    for (var i = 0; i < opts.length; i++) if (opts[i].value === v) return opts[i].label
    return v
  }
  // The summaries stay readable with the master switch off (the row dims
  // instead) — a tab that shows nothing and accepts nothing is not an off
  // state, it is a dead end. The rumble filter is deliberately not listed: it
  // is on in every preset and by default, so saying so distinguishes nothing.
  function cleanupSummary() {
    var m = svc ? svc.micSettings : ({})
    if (!m) return "None"
    var on = []
    if (m.voiceIsolation) on.push("Voice isolation")
    if (m.noiseGate) on.push("Noise gate")
    if (m.autoLevel) on.push("Auto level")
    if (m.deEsser) on.push("De-esser")
    if (m.humFilter) on.push("Hum filter")
    // The rumble filter is on by default and in every preset, so it is not
    // worth a word — unless it has been switched off, which is worth knowing.
    if (!on.length) return m.highPass === false ? "Rumble filter off" : "None"
    return on.length > 2 ? on[0] + " +" + (on.length - 1) : on.join(", ")
  }
  function characterSummary() {
    var m = svc ? svc.micSettings : ({})
    if (!m) return "None"
    var on = []
    if (m.tone && m.tone !== "none") on.push(labelFor(toneOpts, m.tone))
    if (m.voice && m.voice !== "none") on.push(labelFor(voiceOpts, m.voice))
    if (m.space && m.space !== "none") on.push(labelFor(spaceOpts, m.space))
    return on.length ? on.join(" · ") : "None"
  }
  // The master row says which preset this is, or "Custom" — the summaries
  // below it already spell out the detail, twice is enough.
  function presetLabel(key) {   // presetList entries are key/label, not value/label
    for (var i = 0; i < presetList.length; i++) if (presetList[i].key === key) return presetList[i].label
    return key
  }
  function effectsSummary() {
    if (!svc || !svc.micSettings) return ""
    if (activePreset !== "custom") return presetLabel(activePreset)
    return "Custom"
  }
  // Whether the microphone is doing anything at all, for the tab strip's dot.
  readonly property bool micEffectsOn: {
    var m = svc ? svc.micSettings : null
    if (!m || !m.enabled) return false
    return !!(m.voiceIsolation || m.noiseGate || m.autoLevel || m.deEsser || m.humFilter ||
              (m.tone && m.tone !== "none") || (m.voice && m.voice !== "none") || (m.space && m.space !== "none"))
  }
  property bool cleanupOpen: false
  property bool characterOpen: false

  // "Auto" next to the Volume slider: listen to the microphone for a few
  // seconds and set the gain so the loudest thing heard lands a little under
  // the top of the meter. The level it watches is the one before the effects,
  // which is exactly what the volume multiplies.
  property bool autoGainRunning: false
  property real autoGainPeak: 0
  property int autoGainLeft: 0
  readonly property real autoGainTarget: 0.6   // peak, not RMS: leaves headroom
  function startAutoGain() {
    if (!svc || autoGainRunning) return
    autoGainPeak = 0
    autoGainNote = ""
    autoGainLeft = 4
    autoGainRunning = true
    autoGainTimer.start()
  }
  property string autoGainNote: ""
  Timer { id: autoGainNoteClear; interval: 2500; onTriggered: root.autoGainNote = "" }
  function finishAutoGain() {
    autoGainRunning = false
    if (!svc) return
    if (autoGainPeak < 0.01) {   // silence: say so rather than appear to do nothing
      autoGainNote = "heard nothing"
      autoGainNoteClear.restart()
      return
    }
    var gain = autoGainTarget / autoGainPeak
    var v = 0.5 + (20 * Math.log(gain) / Math.LN10) / 36
    svc.setMicSetting("volume", Math.max(0, Math.min(1, Math.round(v * 20) / 20)))
  }
  Timer {
    id: autoGainTimer
    interval: 1000
    repeat: true
    onTriggered: {
      root.autoGainLeft -= 1
      if (root.autoGainLeft <= 0) { stop(); root.finishAutoGain() }
    }
  }
  Connections {   // sample the meter as the daemon pushes it
    target: root.svc
    enabled: root.autoGainRunning
    function onMicInChanged() { if (root.svc.micIn > root.autoGainPeak) root.autoGainPeak = root.svc.micIn }
  }

  function micFooter() {
    if (!svc || !connected) return ""
    // "off" is the daemon's word for "not opened yet", which is the normal
    // state for the second it takes this tab to open the microphone.
    if (svc.micMuted) return "Muted · apps hear silence"
    if (svc.micStatus !== "" && svc.micStatus !== "ok" && svc.micStatus !== "off") return "Microphone problem: " + svc.micStatus
    if (svc.micConsumers > 0) return "In use by " + svc.micConsumers + " app" + (svc.micConsumers > 1 ? "s" : "")
    if (svc.micListening) return "Listening · playing through your speakers"
    return "Idle · select “" + svc.micLabel + "” in your apps"
  }
  function footer() {
    if (!svc || !connected) return ""
    var res = svc.state.output ? " · " + svc.state.output.width + "×" + svc.state.output.height : ""
    // Keep it under ~50 monospace caption characters so it never elides.
    var apps = svc.consumerApps || []
    // Cap the list: the resolution is appended after it, and an unbounded
    // join would push the more durable half out through the elide.
    var named = apps.length > 2 ? apps.slice(0, 2).join(", ") + " +" + (apps.length - 2) : apps.join(", ")
    var use = appCount > 0 ? "In use by " + (apps.length ? named : appCount + " app" + (appCount > 1 ? "s" : ""))
            : previewOnly && inUse ? (blocked ? "Preview only · camera blocked" : "Preview only · camera on while open") : "Idle"
    return use + res
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    // Red for both live states, the way a recording light works: an app is
    // reading the camera, or the microphone is muted — the one state you can
    // reach from a key binding and then forget about.
    text: root.micMuted ? "\uf131" : "󰄀"
    active: root.appsConnected || root.micMuted
    useActiveColor: true
    activeColor: Color.urgent
    tooltipText: (root.micMuted ? "Microphone muted"
                : root.appsConnected ? (root.blocked ? "Camera blocked — apps get the placeholder" : "Camera in use — effects")
                                     : (root.blocked ? "Camera blocked" : "Camera effects")) + " · right-click: Portrait"
    onPressed: function(b) {
      if (b === Qt.RightButton && root.svc) root.svc.setSetting("portrait", !root.s.portrait)
      else root.toggle()
    }
  }

  onOpenedChanged: if (opened && svc) { svc.refresh(); svc.rescan() }

  // Native file chooser for the background image and the block placeholder;
  // falls back to a plain path field when zenity is not around.
  Process {
    id: pickProc
    property string picked: ""
    property int exitCode: -1
    property bool streamDone: false
    property string target: "backgroundImage"   // setting the chosen path goes to
    property string title: "Background image"
    property string filter: "Images | *.png *.jpg *.jpeg *.webp *.bmp"
    command: ["sh", "-c",
      'command -v zenity >/dev/null 2>&1 || exit 127; ' +
      'exec zenity --file-selection --title="$1" --file-filter="$2" --file-filter="All files | *"',
      "camera-effects-pick", title, filter]
    // begin(target, title, filter): open the chooser for `target` (a daemon setting key).
    function begin(t, ti, f) {
      if (running) return
      target = t || "backgroundImage"
      title = ti || "Background image"
      filter = f || "Images | *.png *.jpg *.jpeg *.webp *.bmp"
      picked = ""; exitCode = -1; streamDone = false
      running = true
    }
    function finish() {
      if (exitCode < 0 || !streamDone) return
      if (exitCode === 127) root.pickerMissing = true
      else if (exitCode === 0 && picked !== "" && root.svc) root.svc.setSetting(target, picked)
    }
    stdout: StdioCollector { onStreamFinished: { pickProc.picked = String(text).trim(); pickProc.streamDone = true; pickProc.finish() } }
    onExited: function(code) { pickProc.exitCode = code; pickProc.finish() }
  }

  // Native colour chooser (zenity). GTK prints "rgb(r,g,b)" or "#rrggbb".
  Process {
    id: colorPickProc
    property string initial: "#1e1e2e"
    command: ["sh", "-c", 'command -v zenity >/dev/null 2>&1 || exit 127; exec zenity --color-selection --title="Background colour" --color="$1"', "camera-effects-pick", initial]
    function begin(c) { if (running) return; initial = c; running = true }
    stdout: StdioCollector {
      onStreamFinished: {
        var t = String(text).trim(), hex = ""
        var m = t.match(/^rgba?\((\d+),\s*(\d+),\s*(\d+)/)
        if (m) hex = "#" + [m[1], m[2], m[3]].map(function(v) { var h = Number(v).toString(16); return h.length < 2 ? "0" + h : h }).join("")
        else if (/^#[0-9a-fA-F]{6}$/.test(t)) hex = t.toLowerCase()
        else if (/^#[0-9a-fA-F]{12}$/.test(t)) hex = "#" + t.substr(1, 2) + t.substr(5, 2) + t.substr(9, 2)   // 16-bit per channel
        if (hex !== "" && root.svc) root.svc.setSetting("backgroundColor", hex)
      }
    }
    onExited: function(code) { if (code === 127) root.pickerMissing = true }
  }

  // One compact settings row: label on the left, switch on the right.
  component SwitchRow: Item {
    id: sw
    property string label: ""
    property bool checked: false
    property bool enabled: !!root.svc
    property real indent: 0
    property bool strong: false   // a row that carries its own weight: card + bold label
    property string summary: ""   // dim caption before the switch: what this row amounts to
    signal toggled()
    width: parent ? parent.width : 200
    height: root.rowH
    opacity: enabled ? 1 : 0.5
    Rectangle {  // first child: everything below paints on top of it
      visible: sw.strong
      anchors.fill: parent
      radius: Style.space(8)
      color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, sw.checked ? 0.13 : 0.05)
      border.width: 1
      border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, sw.checked ? 0.3 : 0.12)
    }
    Text {
      id: swSummary
      anchors.right: swToggle.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      visible: sw.summary !== ""
      text: sw.summary
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
      elide: Text.ElideRight
      width: Math.min(implicitWidth, sw.width * 0.5)
      horizontalAlignment: Text.AlignRight
    }
    Text {
      anchors.left: parent.left
      anchors.leftMargin: sw.indent + (sw.strong ? Style.space(10) : 0)
      anchors.right: sw.summary !== "" ? swSummary.left : swToggle.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      text: sw.label
      color: root.fg
      font.family: root.fontFamily
      font.pixelSize: Style.font.body
      font.bold: sw.strong
      elide: Text.ElideRight
    }
    ToggleSwitch {
      id: swToggle
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      cursorPad: root.trailInset
      checked: sw.checked
      interactive: sw.enabled
      foreground: root.fg
      onToggled: sw.toggled()
    }
    MouseArea {  // the whole row toggles, like a real settings list
      anchors.fill: parent
      anchors.rightMargin: swToggle.width
      enabled: sw.enabled
      onClicked: sw.toggled()
    }
  }

  // Intensity slider. `released` fires while dragging too (throttled to ~20 Hz)
  // so the effect follows the handle instead of jumping at the end; the final
  // value is always sent on release.
  component IntensityRow: Item {
    id: intensity
    property real value: 0.6
    property string label: ""       // a bare track tells nobody what more means
    property real indent: Style.space(20)
    signal released(real v)
    width: parent ? parent.width : 200
    height: root.rowH * 0.8
    property real pending: -1
    Timer {
      id: liveThrottle
      interval: 50
      repeat: false
      function flush() { if (intensity.pending >= 0) { intensity.released(intensity.pending); intensity.pending = -1 } }
      onTriggered: flush()
    }
    Text {
      anchors.left: parent.left
      anchors.leftMargin: intensity.indent
      anchors.verticalCenter: parent.verticalCenter
      visible: intensity.label !== ""
      text: intensity.label
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }
    Text {
      anchors.right: intensitySlider.left
      anchors.rightMargin: Style.space(10)
      anchors.verticalCenter: parent.verticalCenter
      visible: intensity.label !== ""
      text: Math.round(intensitySlider.liveValue * 100) + "%"
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }
    PanelSlider {
      id: intensitySlider
      bar: root.bar
      anchors.right: parent.right
      anchors.rightMargin: root.trailInset
      anchors.verticalCenter: parent.verticalCenter
      anchors.left: parent.left
      anchors.leftMargin: intensity.label === "" ? Style.space(20) : Style.space(120)
      height: parent.height
      minimum: 0.05; maximum: 1; step: 0.05
      value: intensity.value
      onMoved: function(v) {
        intensity.pending = v
        if (!liveThrottle.running) { liveThrottle.flush(); liveThrottle.start() }
      }
      onReleased: function(v) { liveThrottle.stop(); intensity.pending = -1; intensity.released(v) }
    }
  }

  // A section that folds away, with what is on inside it written on the header
  // — the summary is the point: it answers "what is this doing" without asking
  // anyone to open anything.
  component SectionHeader: Item {
    id: sec
    property string label: ""
    property string summary: ""
    property bool open: false
    property bool dimmed: false   // what is inside is switched off — still readable, still openable
    signal toggled()
    width: parent ? parent.width : 200
    height: root.rowH
    opacity: dimmed ? 0.55 : 1
    PanelSectionHeader {
      id: secLabel
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      foreground: root.fg
      fontFamily: root.fontFamily
      text: sec.label.toUpperCase()
    }
    Text {
      anchors.left: secLabel.right
      anchors.leftMargin: Style.space(10)
      anchors.right: chevron.left
      anchors.rightMargin: Style.space(6)
      anchors.verticalCenter: parent.verticalCenter
      horizontalAlignment: Text.AlignRight
      text: sec.summary
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
      elide: Text.ElideRight
    }
    Text {
      id: chevron
      anchors.right: parent.right
      anchors.rightMargin: root.trailInset
      anchors.verticalCenter: parent.verticalCenter
      text: sec.open ? "▾" : "▸"
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.body
    }
    MouseArea {
      anchors.fill: parent
      cursorShape: Qt.PointingHandCursor
      onClicked: sec.toggled()
    }
  }

  // A row of chips: every option visible and one click to try it, with the
  // preview (or the microphone) never covered by a popup list — which is the
  // whole reason these are not dropdowns.
  component ChipGrid: Column {
    id: chips
    property string label: ""
    property var options: []
    property string value: "none"
    property bool enabled: true
    signal picked(string v)
    width: parent ? parent.width : 200
    spacing: Style.space(4)
    opacity: enabled ? 1 : 0.5
    Text {
      text: chips.label
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }
    Flow {
      width: chips.width - root.trailInset
      spacing: Style.space(4)
      Repeater {
        model: chips.options
        delegate: Button {
          required property var modelData
          text: modelData.label
          fontSize: Style.font.bodySmall
          fontFamily: root.fontFamily
          foreground: root.fg
          bordered: true
          // (Button bolds its label when selected. The panel's font is
          // monospaced, so the chip keeps its width and the row does not
          // reflow under the pointer that just clicked it.)
          selected: chips.value === modelData.value
          enabled: chips.enabled
          onClicked: chips.picked(modelData.value)
        }
      }
    }
  }

  // One level meter row: caption on the left, a bar that follows the daemon's
  // peak on the right. The bar is the microphone's "preview".
  component LevelBar: Item {
    id: meter
    property string label: ""
    property real value: 0        // 0..1 peak
    property bool live: false     // the microphone is actually open
    property color barColor: Color.accent
    width: parent ? parent.width : 200
    height: root.rowH * 0.7
    Text {
      id: meterLabel
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(40)
      text: meter.label
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }
    Rectangle {
      id: track
      anchors.left: meterLabel.right
      anchors.leftMargin: Style.space(6)
      anchors.right: parent.right
      anchors.rightMargin: root.trailInset
      anchors.verticalCenter: parent.verticalCenter
      height: Style.space(8)
      radius: height / 2
      color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.15)
      Rectangle {
        // Square root of the peak: quiet speech still moves the bar visibly.
        width: Math.max(0, Math.min(1, meter.live ? Math.sqrt(meter.value) : 0)) * track.width
        height: parent.height
        radius: height / 2
        color: meter.value > 0.97 ? "#e05555" : meter.barColor
        Behavior on width { NumberAnimation { duration: 90 } }
      }
    }
  }

  // Dim one-line note (under the preview, under the reactions, footer).
  component Note: Text {
    width: parent ? parent.width : 200
    color: root.dim
    font.family: root.fontFamily
    font.pixelSize: Style.font.caption
    elide: Text.ElideRight
  }

  // Label on the left, a compact dropdown on the right (like the Background row).
  component LabelDropdownRow: Item {
    id: ldr
    property string label: ""
    property var options: []
    property string value: ""
    signal changed(string v)
    width: parent ? parent.width : 200
    height: root.rowH
    opacity: enabled ? 1 : 0.5
    Text {
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      text: ldr.label
      color: root.fg
      font.family: root.fontFamily
      font.pixelSize: Style.font.body
    }
    Dropdown {
      anchors.right: parent.right
      anchors.rightMargin: root.trailInset
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(120)
      showLabel: false
      fontFamily: root.fontFamily
      options: ldr.options
      value: ldr.value
      onChanged: function(v) { ldr.changed(v) }
    }
  }

  readonly property var filterOpts: [ { value: "none", label: "None" }, { value: "mono", label: "Mono" }, { value: "sepia", label: "Sepia" },
    { value: "warm", label: "Warm" }, { value: "cool", label: "Cool" }, { value: "vivid", label: "Vivid" }, { value: "soft", label: "Soft" },
    { value: "sharpen", label: "Sharpen" }, { value: "vintage", label: "Vintage" } ]
  readonly property var funOpts: [ { value: "none", label: "None" }, { value: "sunglasses", label: "Sunglasses" }, { value: "glasses", label: "Glasses" },
    { value: "mask", label: "Mask" }, { value: "tophat", label: "Top hat" }, { value: "crown", label: "Crown" },
    { value: "cat", label: "Cat" }, { value: "halo", label: "Halo" }, { value: "flowers", label: "Flowers" },
    { value: "blur", label: "Blur" }, { value: "pixelate", label: "Pixelate" } ]
  readonly property var ambienceOpts: [ { value: "none", label: "None" }, { value: "rain", label: "Rain" }, { value: "snow", label: "Snow" },
    { value: "sparkles", label: "Sparkles" }, { value: "confetti", label: "Confetti" }, { value: "bubbles", label: "Bubbles" } ]
  readonly property var fitOpts: [ { value: "cover", label: "Cover" }, { value: "contain", label: "Contain" }, { value: "stretch", label: "Stretch" } ]
  readonly property var rotateOpts: [ { value: "0", label: "Off" }, { value: "90", label: "90°" }, { value: "180", label: "180°" }, { value: "270", label: "270°" } ]
  readonly property string imageFilter: "Images | *.png *.jpg *.jpeg *.webp *.bmp"
  readonly property string videoFilter: "Videos | *.mp4 *.mkv *.webm *.mov *.avi *.y4m *.gif"
  // Block placeholder kind: derived from the source's extension ("card" when
  // empty); blockKindPick remembers a kind chosen before its file is picked.
  function blockKindOf(p) {
    var e = String(p || "").toLowerCase().replace(/^.*\./, "")
    if (!p) return "card"
    return ["png", "jpg", "jpeg", "webp", "bmp"].indexOf(e) !== -1 ? "image" : "video"
  }
  property string blockKindPick: ""
  readonly property string blockSourceNow: svc ? svc.blockSource : ""
  onBlockSourceNowChanged: blockKindPick = ""
  readonly property string blockKind: blockKindPick !== "" ? blockKindPick : blockKindOf(blockSourceNow)

  // Everything on the card except the preview, in the card's own coordinates:
  // what the preview must leave room for (see previewBox.roomH). The tab
  // content is the only part that changes, so the card is exactly as tall as
  // the tab in front.
  readonly property real restH: topBlock.implicitHeight + Style.space(6) + previewNote.blockH + Style.space(10)
                                + tabStrip.height + scopeNote.blockH + Style.space(8)
                                + body.implicitHeight + footerBlock.height

  // The settings live in five tabs; the panel remembers the last one for as
  // long as it stays loaded (nothing persisted: a fresh panel starts on Video).
  readonly property var tabs: [ { key: "video", label: "Video" }, { key: "look", label: "Look" },
                                { key: "fun", label: "Fun" }, { key: "mic", label: "Mic" },
                                { key: "privacy", label: "Privacy" } ]
  property string tab: "video"

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    // Hero and preview stay put at the top; under them a tab strip and the
    // active tab's rows, which scroll when the screen is short. The screen
    // governs both axes (no cap, like network/bluetooth); the status footer is
    // pinned under the ScrollView so it never scrolls or clips away.
    contentWidth: panel.fittedContentWidth(Style.space(400))
    contentHeight: panel.fittedContentHeight(root.restH + previewRow.height)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      blocked: root.resetConfirm || root.micResetConfirm   // the dialog owns the keyboard while it is up
      onCloseRequested: root.userClose()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      Keys.onPressed: function(event) {
        if (resetDialog.handleKey(event)) event.accepted = true
        else if (micResetDialog.handleKey(event)) event.accepted = true
      }

      // Reset asks first: it clears every effect of the selected camera.
      ConfirmDialog {
        id: resetDialog
        anchors.fill: parent
        z: 10
        opened: root.resetConfirm
        message: "Reset all effects for this camera?"
        confirmText: "Reset"
        foreground: root.fg
        fontFamily: root.fontFamily
        onCanceled: root.resetConfirm = false
        onConfirmed: { root.resetConfirm = false; if (root.svc) root.svc.reset() }
      }

      ConfirmDialog {
        id: micResetDialog
        anchors.fill: parent
        z: 10
        opened: root.micResetConfirm
        message: "Reset this microphone's effects and volume?"
        confirmText: "Reset"
        foreground: root.fg
        fontFamily: root.fontFamily
        onCanceled: root.micResetConfirm = false
        onConfirmed: { root.micResetConfirm = false; if (root.svc) root.svc.micReset() }
      }

      // ---------- Fixed top: hero · setup · preview ----------
      Column {
        id: topBlock
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Style.space(6)

        // ---------- Hero ----------
        PanelHero {
          width: parent.width
          title: root.svc ? root.svc.loopbackLabel : "Camera Effects"
          meta: root.subtitle()
          foreground: root.fg
          fontFamily: root.fontFamily
          iconComponent: Component {
            Text {
              text: "󰄀"
              color: root.appsConnected ? Color.accent : root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.display
            }
          }
        }
        Item { width: parent.width; height: Style.space(4) }  // hero → preview breathing room

        // ---------- Not installed / needs setup / needs rebuild ----------
        Row {
          width: parent.width
          spacing: Style.space(8)
          visible: root.svc && !root.svc.setupBusy && (!root.svc.installed || root.svc.deviceMissing || root.needsRebuild)
          Button {
            text: root.svc && !root.svc.installed ? "Install (build daemon)"
                : root.needsRebuild ? "Rebuild daemon" : "Set up virtual camera"
            foreground: root.fg
            fontFamily: root.fontFamily
            bordered: true
            onClicked: {
              if (!root.svc) return
              if (!root.svc.installed || root.needsRebuild) root.svc.install()
              else root.svc.runSetup("install")
            }
          }
        }
        Text {
          width: parent.width
          visible: root.svc && (root.svc.busyText !== "" || root.svc.setupOutput !== "" || (root.needsRebuild && root.svc.daemonLog !== ""))
          wrapMode: Text.WordWrap
          text: root.svc ? (root.svc.busyText !== "" ? root.svc.busyText
                            : root.svc.setupOutput !== "" ? root.svc.setupOutput
                            : root.svc.daemonLog.trim().split("\n").slice(-3).join("\n")) : ""
          color: root.dim
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
        }
      }

      Item {  // preview row: full width; the box itself may be narrower (see previewBox)
        id: previewRow
        anchors.top: topBlock.bottom
        anchors.topMargin: Style.space(6)
        anchors.left: parent.left
        anchors.right: parent.right
        height: previewBox.visible ? previewBox.height : 0

        Rectangle {
          id: previewBox
          // Full width at 16:9 when the screen allows; on a short screen it
          // shrinks (still 16:9, centred) so the columns below fit in their
          // default state, down to a floor — past that the columns scroll.
          readonly property int fullH: Math.round(parent.width * 9 / 16)
          readonly property int roomH: Math.floor(panel.availableCardHeight - panel.verticalContentInset - root.restH)
          height: Math.max(Style.space(140), Math.min(fullH, roomH))
          width: height >= fullH ? parent.width : Math.round(height * 16 / 9)
          anchors.horizontalCenter: parent.horizontalCenter
          radius: Style.cornerRadius
          color: "#000000"
          clip: true
          // Not on the Mic tab: nothing there is about the picture, and the
          // preview is what holds the camera open.
          visible: root.connected && root.svc && root.svc.loopback !== "" && root.tab !== "mic"

          Loader {
            id: previewLoader
            anchors.fill: parent
            // Only while the panel is on screen: previewActive asks the
            // daemon to run the pipeline (camera on) and write the preview.
            active: root.opened && previewBox.visible
            source: Qt.resolvedUrl("Preview.qml")
            onLoaded: {
              item.path = root.svc.previewPath
              // Keep the preview a mirror view of yourself whatever the
              // output setting is: undo the flip only when the feed
              // itself is already mirrored.
              // Self-view convention: the preview is a mirror unless the user turns it off; the
              // placeholder is never flipped. This is display-only — apps get the unflipped output
              // (or the mirrored one when "Mirror output" is on).
              item.mirror = Qt.binding(function() { return !root.blocked && root.svc && root.svc.previewMirror })
            }
          }
          // Black until the daemon delivers its first live frame.
          Rectangle {
            anchors.fill: parent
            color: "#000000"
            visible: !root.inUse || !previewLoader.item || !previewLoader.item.ready
            Column {
              anchors.centerIn: parent
              spacing: Style.space(8)
              Text {
                id: startSpinner
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.busy
                text: "󰑐"
                color: Qt.rgba(1, 1, 1, 0.7)
                font.family: root.fontFamily
                font.pixelSize: Style.font.title
                RotationAnimation on rotation {
                  running: startSpinner.visible
                  loops: Animation.Infinite
                  from: 0; to: 360; duration: 1200
                }
              }
              Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.busy ? "Starting camera…"
                    : root.connected && root.svc && root.svc.camera && root.svc.camera.name ? "Camera idle" : "No camera"
                color: Qt.rgba(1, 1, 1, 0.5)  // the box is always black
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
            }
          }
          // The camera is delivering frames, but they are black — almost always a
          // closed privacy cover on the webcam.
          Rectangle {
            anchors.centerIn: parent
            visible: previewBox.visible && root.svc && root.svc.covered && !root.blocked
            width: Math.min(parent.width - Style.space(24), coveredText.implicitWidth + Style.space(24))
            height: coveredText.implicitHeight + Style.space(14)
            radius: Style.space(6)
            color: Qt.rgba(0, 0, 0, 0.6)
            Text {
              id: coveredText
              anchors.centerIn: parent
              text: "Something might be blocking your camera"
              color: "#ffffff"
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
          }
          // Reopening (Center Stage flips the capture size, or the camera is
          // switched): keep the last frame, dim it and spin over it instead of
          // looking frozen.
          Rectangle {
            anchors.fill: parent
            visible: root.busy && !!previewLoader.item && previewLoader.item.ready
            color: Qt.rgba(0, 0, 0, 0.45)
            Text {
              id: reopenSpinner
              anchors.centerIn: parent
              text: "󰑐"
              color: Qt.rgba(1, 1, 1, 0.85)
              font.family: root.fontFamily
              font.pixelSize: Style.font.title
              RotationAnimation on rotation {
                running: reopenSpinner.visible
                loops: Animation.Infinite
                from: 0; to: 360; duration: 1200
              }
            }
          }
          MouseArea {  // drag pans, wheel zooms, double-click resets (the camera's framing, or the placeholder's while blocked)
            id: panArea
            anchors.fill: parent
            preventStealing: true   // a vertical pan drag is not a scroll (the ScrollView's Flickable would grab it)
            enabled: root.framingEnabled && !!previewLoader.item
            acceptedButtons: Qt.LeftButton
            cursorShape: !enabled || !root.panEnabled ? Qt.ArrowCursor : (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
            property real startX: 0
            property real startY: 0
            property real panX0: 0
            property real panY0: 0
            property var pending: null
            onPressed: function(mouse) { startX = mouse.x; startY = mouse.y; panX0 = root.panXNow; panY0 = root.panYNow }
            onPositionChanged: function(mouse) {
              if (!pressed || !root.panEnabled || !root.svc) return
              var r = root.svc.panRange
              var rx = r[0] || 0, ry = r[1] || 0
              if (rx <= 0 && ry <= 0) return
              // pan = the crop's place in its free range (-1..1); the range covers 2 * r * (preview size) px.
              // The preview shows the output as apps get it: dragging the picture right moves the crop left
              // (-1); when the output is mirrored the picture is flipped, so the sign flips too. Rotation
              // needs no correction: pan applies to the rotated frame, which is what the preview shows.
              var flipped = !root.blocked && ((!!root.s.mirror) !== (!!(root.svc && root.svc.previewMirror)))
              var sx = flipped ? 1 : -1
              var px = rx > 0 ? Math.max(-1, Math.min(1, panX0 + sx * (mouse.x - startX) / (rx * width))) : panX0
              var py = ry > 0 ? Math.max(-1, Math.min(1, panY0 - (mouse.y - startY) / (ry * height))) : panY0
              pending = { panX: Math.round(px * 1000) / 1000, panY: Math.round(py * 1000) / 1000 }
              if (!panThrottle.running) { panThrottle.flush(); panThrottle.start() }  // ~20 Hz
            }
            onReleased: panThrottle.flush()
            onDoubleClicked: root.setFraming({ zoom: 1, panX: 0, panY: 0 })
            onWheel: function(wheel) {
              var z = Math.round(Math.max(1, Math.min(4, root.zoomLive + (wheel.angleDelta.y > 0 ? 0.1 : -0.1))) * 10) / 10
              if (z === root.zoomLive) return
              root.zoomLive = z
              zoomTouched.restart()
              root.setFraming({ zoom: z })
            }
            Timer {
              id: panThrottle
              interval: 50
              function flush() { if (panArea.pending) { root.setFraming(panArea.pending); panArea.pending = null } }
              onTriggered: flush()
            }
          }
          Rectangle {  // gesture the daemon currently sees, and what it will trigger
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: Style.space(8)
            visible: !!root.svc && !!root.s.reactions && root.svc.gesture !== "" && !badgeHide.running && !!previewLoader.item && previewLoader.item.ready
            color: "#aa000000"
            radius: Style.space(4)
            width: gestureText.implicitWidth + Style.space(12)
            height: gestureText.implicitHeight + Style.space(6)
            Text {
              id: gestureText
              anchors.centerIn: parent
              text: root.svc ? root.gestureTitle(root.svc.gesture) + " → " + root.effectTitle(root.svc.gesture) : ""
              color: "#fff"
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
          }
          Rectangle {  // shutter button: snapshot of what apps see (after a 3 s countdown)
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Style.space(8)
            visible: !!root.svc && root.snapCount === 0 && !!previewLoader.item && previewLoader.item.ready
            color: snapArea.containsMouse ? "#cc000000" : "#aa000000"
            radius: Style.space(4)
            width: snapGlyph.implicitWidth + Style.space(12)
            height: snapGlyph.implicitHeight + Style.space(6)
            Text {
              id: snapGlyph
              anchors.centerIn: parent
              text: "󰄀"
              color: "#fff"
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
            }
            MouseArea {
              id: snapArea
              anchors.fill: parent
              hoverEnabled: true
              cursorShape: Qt.PointingHandCursor
              onClicked: root.snap()
            }
            PanelToolTip { visible: snapArea.containsMouse; text: "Snapshot · saved to ~/Pictures/Camera Effects and copied"; fontFamily: root.fontFamily }
          }
          Rectangle {  // countdown: a big centred number on a dim backdrop
            anchors.centerIn: parent
            visible: root.snapCount > 0
            color: "#aa000000"
            radius: Style.space(8)
            width: Math.max(snapCountText.implicitHeight, snapCountText.implicitWidth) + Style.space(24)
            height: width
            Text {
              id: snapCountText
              anchors.centerIn: parent
              text: root.snapCount > 0 ? String(root.snapCount) : ""
              color: "#fff"
              font.family: root.fontFamily
              font.pixelSize: Style.font.display * 2
              font.bold: true
            }
          }
          Rectangle {  // shutter flash when the snapshot is saved
            id: snapFlashRect
            anchors.fill: parent
            color: "#ffffff"
            opacity: 0
            NumberAnimation { id: snapFlash; target: snapFlashRect; property: "opacity"; from: 0.85; to: 0; duration: 400; easing.type: Easing.OutQuad }
          }
        }
      }
      // Its height comes from the font, not the Text's implicit size: that
      // size feeds the card height (root.restH), and a Text's implicitHeight
      // read inside that synchronous chain trips the binding-loop detector.
      FontMetrics { id: noteMetrics; font.family: root.fontFamily; font.pixelSize: Style.font.caption }
      Note {
        id: previewNote
        readonly property real blockH: visible ? height + Style.space(6) : 0
        anchors.top: previewRow.bottom
        anchors.topMargin: visible ? Style.space(6) : 0
        height: visible ? Math.ceil(noteMetrics.height) : 0
        visible: previewBox.visible && root.previewActive && (root.centerStageOn || root.panEnabled)
        text: root.centerStageOn ? "Center Stage frames automatically" : "Drag to pan · scroll to zoom"
      }

      // ---------- Tabs: Video · Look · Fun · Mic · Privacy ----------
      Item {
        id: tabStrip
        anchors.top: previewNote.bottom
        anchors.topMargin: Style.space(10)
        anchors.left: parent.left
        anchors.right: parent.right
        // A fixed height, not the labels' implicit one: this feeds root.restH,
        // and a Text's implicitHeight read inside that chain trips the
        // binding-loop detector (same reason as previewNote above).
        height: Style.space(26)

        PanelSeparator {  // baseline the active tab's underline sits on
          anchors.bottom: parent.bottom
          foreground: root.fg
        }
        Row {
          anchors.fill: parent
          Repeater {
            model: root.tabs
            delegate: Item {
              id: tabItem
              required property var modelData
              readonly property bool current: root.tab === tabItem.modelData.key
              width: tabStrip.width / root.tabs.length
              height: tabStrip.height
              Text {
                id: tabLabel
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -Style.space(1)
                text: tabItem.modelData.label
                color: tabItem.current ? Color.accent : (tabArea.containsMouse ? root.fg : root.dim)
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
                font.bold: tabItem.current
              }
              Rectangle {  // a dot when that tab is doing something you cannot see from here
                anchors.left: tabLabel.right
                anchors.leftMargin: Style.space(3)
                anchors.top: tabLabel.top
                anchors.topMargin: Style.space(2)
                width: Style.space(4); height: width
                radius: width / 2
                color: root.micMuted ? Color.urgent : Color.accent
                visible: tabItem.modelData.key === "mic" && (root.micEffectsOn || root.micMuted) && !tabItem.current
              }
              Rectangle {  // 2px accent underline under the active label
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.round(tabLabel.implicitWidth) + Style.space(12)
                height: Style.space(2)
                radius: height / 2
                color: Color.accent
                visible: tabItem.current
              }
              MouseArea {
                id: tabArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.tab = tabItem.modelData.key
              }
            }
          }
        }
      }
      // Which camera the effect tabs are editing, when the cameras do not
      // share their settings. Fixed height, like previewNote and for the same
      // reason (it feeds root.restH).
      Note {
        id: scopeNote
        readonly property real blockH: visible ? height + Style.space(4) : 0
        anchors.top: tabStrip.bottom
        anchors.topMargin: visible ? Style.space(4) : 0
        height: visible ? Math.ceil(noteMetrics.height) : 0
        visible: root.multiCam && !!root.svc && !root.svc.sameForAll && root.tab !== "privacy" && root.tab !== "mic"
        text: root.svc ? "Settings for " + root.shortName(root.svc.camera) : ""
      }

      // ---------- Tab content ----------
      // All five Columns stay built so switching tabs is instant; only the
      // active one is visible, and only its height counts towards the card
      // (an invisible Column is skipped by its parent's layout).
      ScrollView {
        id: scrollArea
        anchors.top: scopeNote.bottom
        anchors.topMargin: Style.space(8)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footerBlock.top
        clip: true
        readonly property bool overflows: body.implicitHeight > height + 1  // +1: rounding slack
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: overflows ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        // The ScrollView's Flickable takes the wheel (and can be dragged) only
        // while interactive; tie that to overflow, like the audio panel, so a
        // short list never bounces. The preview keeps its own wheel (zoom).
        Binding {
          target: scrollArea.contentItem
          property: "interactive"
          value: scrollArea.overflows
        }

        Item {
          id: body
          width: scrollArea.availableWidth
          implicitWidth: width
          implicitHeight: videoCol.visible ? videoCol.implicitHeight
                        : lookCol.visible ? lookCol.implicitHeight
                        : funCol.visible ? funCol.implicitHeight
                        : micCol.visible ? micCol.implicitHeight
                        : privacyCol.implicitHeight

          // ---------- Video: camera · framing ----------
          Column {
            id: videoCol
            width: body.width
            visible: root.tab === "video"
            spacing: Style.space(6)

            Dropdown {
              width: parent.width - root.trailInset
              visible: !!root.svc && (root.multiCam ||
                       (!!root.svc.camera && !!root.svc.camera.bus && !root.cams.some(function(c) { return c.bus === root.svc.camera.bus })))
              showLabel: false
              fontFamily: root.fontFamily
              options: root.cameraOpts
              value: root.svc && root.svc.camera && root.svc.camera.bus ? String(root.svc.camera.bus) : ""
              onChanged: function(v) { if (root.svc) root.svc.selectCamera(v) }
            }
            SwitchRow {
              visible: root.multiCam
              label: "Same effects on every camera"
              checked: root.svc ? root.svc.sameForAll : true
              onToggled: if (root.svc) root.svc.setSameForAll(!root.svc.sameForAll)
            }
            SwitchRow { label: "Center Stage"; checked: !!root.s.centerStage; onToggled: if (root.svc) root.svc.setSetting("centerStage", !root.s.centerStage) }
            IntensityRow {  // how tight / how eagerly it follows: left = calm and wide, right = tight and snappy
              visible: !!root.s.centerStage
              value: root.s.centerStageIntensity !== undefined ? root.s.centerStageIntensity : 0.5
              onReleased: function(v) { if (root.svc) root.svc.setSetting("centerStageIntensity", v) }
            }
            Item {  // Zoom: label · value · slider (right-click the slider resets; the preview's wheel steps it too)
              width: parent.width
              height: root.rowH
              enabled: root.framingEnabled
              opacity: enabled ? 1 : 0.5
              Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Zoom"
                color: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
              }
              Text {
                anchors.right: zoomSlider.left
                anchors.rightMargin: Style.space(10)
                anchors.verticalCenter: parent.verticalCenter
                text: zoomSlider.liveValue.toFixed(1) + "×"
                color: root.dim
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
              PanelSlider {
                id: zoomSlider
                bar: root.bar
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                width: Style.space(120)
                minimum: 1; maximum: 4; step: 0.1
                value: root.zoomNow
                property real pending: -1
                Timer {
                  id: zoomThrottle
                  interval: 50
                  repeat: false
                  function flush() { if (zoomSlider.pending >= 0) { root.setFraming({ zoom: Math.round(zoomSlider.pending * 10) / 10 }); zoomSlider.pending = -1 } }
                  onTriggered: flush()
                }
                onMoved: function(v) {   // follow the handle while dragging (throttled), not only on release
                  zoomSlider.pending = v
                  if (!zoomThrottle.running) { zoomThrottle.flush(); zoomThrottle.start() }
                }
                onReleased: function(v) { zoomThrottle.stop(); zoomSlider.pending = -1; root.setFraming({ zoom: Math.round(v * 10) / 10 }) }
                onRightClicked: root.setFraming({ zoom: 1 })
              }
            }
            LabelDropdownRow {  // how the source is placed on the output (Center Stage frames on its own)
              label: "Fit"
              options: root.fitOpts
              value: root.fitNow
              enabled: root.framingEnabled && !root.centerStageOn
              onChanged: function(v) { root.setFraming({ fit: v }) }
            }
            LabelDropdownRow {  // clockwise, before everything else (the placeholder is not rotated)
              label: "Rotate"
              options: root.rotateOpts
              value: String(root.s.rotate || 0)
              enabled: !!root.svc && !root.blocked
              onChanged: function(v) { if (root.svc) root.svc.setSetting("rotate", parseInt(v, 10)) }
            }
            SwitchRow { label: "Mirror preview (only here)"; checked: root.svc ? root.svc.previewMirror : true; onToggled: if (root.svc) root.svc.send({ cmd: "set", settings: { previewMirror: !root.svc.previewMirror } }) }
            SwitchRow { label: "Mirror output (for everyone)"; checked: !!root.s.mirror; onToggled: if (root.svc) root.svc.setSetting("mirror", !root.s.mirror) }
            Item {  // every effect of this camera back to the built-in defaults (the global privacy settings stay)
              width: parent.width
              height: root.rowH
              Button {
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                text: "Reset"
                fontSize: Style.font.bodySmall
                foreground: root.fg
                fontFamily: root.fontFamily
                bordered: true
                enabled: !!root.svc
                opacity: enabled ? 1 : 0.5
                onClicked: root.resetConfirm = true
              }
            }
          }

          // ---------- Look: portrait · light · background · filter ----------
          Column {
            id: lookCol
            width: body.width
            visible: root.tab === "look"
            spacing: Style.space(6)

            SwitchRow { label: "Portrait"; checked: !!root.s.portrait; onToggled: if (root.svc) root.svc.setSetting("portrait", !root.s.portrait) }
            IntensityRow {
              visible: !!root.s.portrait
              value: root.s.portraitIntensity !== undefined ? root.s.portraitIntensity : 0.6
              onReleased: function(v) { if (root.svc) root.svc.setSetting("portraitIntensity", v) }
            }
            SwitchRow { label: "Studio Light"; checked: !!root.s.studioLight; onToggled: if (root.svc) root.svc.setSetting("studioLight", !root.s.studioLight) }
            IntensityRow {
              visible: !!root.s.studioLight
              value: root.s.studioLightIntensity !== undefined ? root.s.studioLightIntensity : 0.6
              onReleased: function(v) { if (root.svc) root.svc.setSetting("studioLightIntensity", v) }
            }
            Item {  // Background: label · [swatch + hex] · mode dropdown
              width: parent.width
              height: root.rowH
              Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Background"
                color: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
              }
              Row {
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                spacing: Style.space(6)
                Rectangle {  // live swatch of the colour in the field
                  visible: root.s.background === "color"
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(14); height: Style.space(14)
                  radius: Style.space(4)
                  color: colorField.valid ? colorField.text : "transparent"
                  border.width: 1
                  border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.35)
                }
                TextField {
                  id: colorField
                  readonly property bool valid: /^#[0-9a-fA-F]{6}$/.test(text)   // daemon accepts #RRGGBB only
                  visible: root.s.background === "color"
                  width: Style.space(96)
                  height: bgDropdown.height
                  verticalAlignment: TextInput.AlignVCenter
                  foreground: root.fg
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.body
                  placeholderText: "#1e1e2e"
                  // Not a binding: state pushes arrive several times a second and would
                  // overwrite what is being typed. Follow the setting only while unfocused.
                  Component.onCompleted: text = root.s.backgroundColor || ""
                  Connections { target: root; function onSChanged() { if (!colorField.activeFocus) colorField.text = root.s.backgroundColor || "" } }
                  onEditingFinished: {
                    var t = text.trim()
                    if (t !== "" && t[0] !== "#") t = "#" + t
                    if (/^#[0-9a-fA-F]{3}$/.test(t)) t = "#" + t[1] + t[1] + t[2] + t[2] + t[3] + t[3]
                    if (root.svc && /^#[0-9a-fA-F]{6}$/.test(t)) { text = t.toLowerCase(); root.svc.setSetting("backgroundColor", text) }
                  }
                }
                Dropdown {
                  id: bgDropdown
                  width: Style.space(96)
                  showLabel: false
                  fontFamily: root.fontFamily
                  options: [ { value: "none", label: "None" }, { value: "color", label: "Color" }, { value: "image", label: "Image" }, { value: "video", label: "Video" } ]
                  value: root.s.background || "none"
                  onChanged: function(v) { if (root.svc) root.svc.setSetting("background", v) }
                }
              }
            }
            Item {  // Background colour: preset swatches + native picker (own row, indented like the sliders)
              visible: root.s.background === "color"
              width: parent.width
              height: root.rowH
              Row {
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Style.space(6)
                Repeater {
                  model: ["#000000", "#ffffff", "#1e1e2e", "#3b4252", "#1e5a8a", "#2e7d32", "#6a1b9a", "#b71c1c", "#e65100", "#00897b"]
                  delegate: Rectangle {
                    required property string modelData
                    width: Style.space(18); height: Style.space(18)
                    radius: Style.space(4)
                    color: modelData
                    border.width: (root.s.backgroundColor || "").toLowerCase() === modelData ? 2 : 1
                    border.color: (root.s.backgroundColor || "").toLowerCase() === modelData ? Color.accent : Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.35)
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: if (root.svc) root.svc.setSetting("backgroundColor", modelData) }
                  }
                }
              }
              Button {
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                text: "Pick…"
                foreground: root.fg
                fontFamily: root.fontFamily
                bordered: true
                visible: !root.pickerMissing
                onClicked: colorPickProc.begin(root.s.backgroundColor || "#1e1e2e")
              }
            }
            Item {  // Background image: elided file name + chooser (own row, indented like the sliders)
              visible: root.s.background === "image"
              width: parent.width
              height: root.rowH
              Note {
                visible: !root.pickerMissing
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.right: chooseBtn.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideMiddle
                font.pixelSize: Style.font.bodySmall
                color: root.s.backgroundImage ? root.fg : root.dim
                text: root.s.backgroundImage ? root.basename(root.s.backgroundImage) : "No image chosen"
              }
              TextField {  // fallback when there is no zenity to pick with
                visible: root.pickerMissing
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.right: chooseBtn.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                foreground: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                placeholderText: "/path/to/image.png"
                text: root.s.backgroundImage || ""
                onEditingFinished: if (root.svc) root.svc.setSetting("backgroundImage", text)
              }
              Button {
                id: chooseBtn
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                text: "Choose…"
                fontSize: Style.font.bodySmall
                foreground: root.fg
                fontFamily: root.fontFamily
                bordered: true
                enabled: !root.pickerMissing
                opacity: enabled ? 1 : 0.5
                onClicked: pickProc.begin("backgroundImage", "Background image", root.imageFilter)
              }
            }
            Item {  // Background video: elided file name + chooser (own row, like the image one)
              visible: root.s.background === "video"
              width: parent.width
              height: root.rowH
              Note {
                visible: !root.pickerMissing
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.right: chooseVideoBtn.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideMiddle
                font.pixelSize: Style.font.bodySmall
                color: root.s.backgroundVideo ? root.fg : root.dim
                text: root.s.backgroundVideo ? root.basename(root.s.backgroundVideo) : "No video chosen"
              }
              TextField {  // fallback when there is no zenity to pick with
                visible: root.pickerMissing
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.right: chooseVideoBtn.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                foreground: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                placeholderText: "/path/to/video.mp4"
                text: root.s.backgroundVideo || ""
                onEditingFinished: if (root.svc) root.svc.setSetting("backgroundVideo", text)
              }
              Button {
                id: chooseVideoBtn
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                text: "Choose…"
                fontSize: Style.font.bodySmall
                foreground: root.fg
                fontFamily: root.fontFamily
                bordered: true
                enabled: !root.pickerMissing
                opacity: enabled ? 1 : 0.5
                onClicked: pickProc.begin("backgroundVideo", "Background video", root.videoFilter)
              }
            }
            LabelDropdownRow { label: "Filter"; options: root.filterOpts; value: root.s.filter || "none"; onChanged: function(v) { if (root.svc) root.svc.setSetting("filter", v) } }
          }

          // ---------- Fun: face accessories · reactions ----------
          Column {
            id: funCol
            width: body.width
            visible: root.tab === "fun"
            spacing: Style.space(6)

            LabelDropdownRow { label: "Effects"; options: root.funOpts; value: root.s.fun || "none"; onChanged: function(v) { if (root.svc) root.svc.setSetting("fun", v) } }
            LabelDropdownRow {  // endless particles over everything (rain, snow, …)
              label: "Ambience"
              options: root.ambienceOpts
              value: root.s.ambience || "none"
              onChanged: function(v) { if (root.svc) root.svc.setSetting("ambience", v) }
            }
            SwitchRow {  // switch = hand-gesture triggers; the strip below plays one now
              label: "Reactions · hand gestures"
              checked: !!root.s.reactions
              onToggled: if (root.svc) root.svc.setSetting("reactions", !root.s.reactions)
            }
            Column {
              width: parent.width
              spacing: Style.space(2)
              Row {
                x: Style.space(20)
                spacing: Style.space(6)
                Repeater {
                  model: root.reactionList
                  delegate: Item {
                    required property string modelData
                    width: Style.space(24); height: Style.space(24)
                    Text { anchors.centerIn: parent; text: root.glyph(modelData); font.pixelSize: Style.space(14) }
                    MouseArea {
                      id: reactArea
                      anchors.fill: parent
                      hoverEnabled: true
                      enabled: !!root.svc
                      onClicked: if (root.svc) { root.svc.react(modelData); badgeHide.restart() }
                      cursorShape: Qt.PointingHandCursor
                    }
                    Rectangle { anchors.fill: parent; radius: Style.space(4); color: root.fg; opacity: reactArea.containsMouse ? 0.12 : 0; z: -1 }
                    PanelToolTip { visible: reactArea.containsMouse; text: root.hint(modelData); fontFamily: root.fontFamily }
                  }
                }
              }
              Note {
                x: Style.space(20)
                width: parent.width - Style.space(20)
                text: "Click to play"
              }
            }
          }

          // ---------- Mic: microphone · effects · privacy ----------
          // The same story as the camera, for audio: pick the real microphone,
          // apply effects, and every app picks "Microphone Effects" instead.
          Column {
            id: micCol
            width: body.width
            visible: root.tab === "mic"
            spacing: Style.space(6)

            Dropdown {  // like the camera's: only worth a row when there is a choice
              width: parent.width - root.trailInset
              visible: root.multiMic || (!!root.svc && root.svc.micWanted !== "")
              showLabel: false
              fontFamily: root.fontFamily
              options: root.micOpts
              value: root.svc ? root.svc.micWanted : ""
              onChanged: function(v) { if (root.svc) root.svc.selectMic(v) }
            }
            Note {  // only when the dropdown above is not already saying it
              visible: !!root.svc && root.svc.micWanted === "" && !!root.svc.micSource && !!root.svc.micSource.description
              text: root.svc && root.svc.micSource ? "Using " + root.svc.micSource.description : ""
              width: parent.width - root.trailInset
            }
            SwitchRow {
              visible: root.multiMic
              label: "Same effects on every microphone"
              checked: root.svc ? root.svc.micSameForAll : true
              onToggled: if (root.svc) root.svc.setMicSameForAll(!root.svc.micSameForAll)
            }
            Note {  // a headset mic only works in the call profile, and that costs playback quality
              visible: !!root.svc && !!root.svc.micSource && !!root.svc.micSource.bluetooth
              text: "Bluetooth: playback drops to call quality while this microphone is open."
              wrapMode: Text.WordWrap
              width: parent.width - root.trailInset
            }
            Column {  // levels: what the microphone hears, and what apps get
              width: parent.width
              spacing: Style.space(2)
              LevelBar {
                label: "Before"
                value: root.svc ? root.svc.micIn : 0
                live: !!root.svc && root.svc.micCapturing
                barColor: root.fg
              }
              LevelBar {
                label: "After"
                value: root.svc ? root.svc.micOut : 0
                live: !!root.svc && root.svc.micCapturing && !root.svc.micMuted
              }
              Note { text: root.micFooter(); width: parent.width - root.trailInset }
            }
            SwitchRow {  // privacy shutter for the microphone: apps get silence
              label: "Mute microphone"
              checked: !!root.svc && root.svc.micMuted
              onToggled: if (root.svc) root.svc.setMicMuted(!root.svc.micMuted)
            }
            SwitchRow {  // hear what apps get, through the speakers
              label: "Listen to this microphone"
              strong: true
              checked: !!root.svc && root.svc.micListen
              enabled: !!root.svc && !root.svc.micMuted
              onToggled: if (root.svc) root.svc.setMicListen(!root.svc.micListen)
            }
            Note {  // one line in every state: a note that changes height moves the rows under the pointer
              text: root.svc && root.svc.micMuted ? "Muted — unmute to hear yourself."
                  : root.svc && root.svc.micListen ? "Use headphones — speakers will echo."
                  : "Hear what apps hear · only while this tab is open"
              width: parent.width - root.trailInset
            }
            Item {  // Volume: label · value · slider (right-click resets to 0 dB).
              // The microphone's own gain, not an effect: it works with the
              // effects switched off, which is why it sits above them.
              width: parent.width
              height: root.rowH
              enabled: !!root.svc
              opacity: enabled ? 1 : 0.5
              Text {
                id: volumeLabel
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Volume"
                color: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
              }
              Button {  // set it from what the microphone actually hears
                anchors.left: volumeLabel.right
                anchors.leftMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                text: "Auto"
                fontSize: Style.font.caption
                fontFamily: root.fontFamily
                foreground: root.fg
                bordered: true
                enabled: !!root.svc && root.svc.micCapturing && !root.svc.micMuted && !root.autoGainRunning
                opacity: enabled ? 1 : 0.5
                tooltipText: "Talk normally for a few seconds; the volume is set from what is heard"
                onClicked: root.startAutoGain()
              }
              Text {
                anchors.right: volSlider.left
                anchors.rightMargin: Style.space(10)
                anchors.verticalCenter: parent.verticalCenter
                text: root.autoGainRunning ? "say something… " + root.autoGainLeft
                     : root.autoGainNote !== "" ? root.autoGainNote
                     : (volSlider.liveValue >= 0.5 ? "+" : "") + Math.round((volSlider.liveValue - 0.5) * 36) + " dB"
                color: root.dim
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
              PanelSlider {
                id: volSlider
                bar: root.bar
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                width: Style.space(120)
                minimum: 0; maximum: 1; step: 0.05
                value: root.m.volume !== undefined ? root.m.volume : 0.5
                property real pending: -1
                Timer {
                  id: volThrottle
                  interval: 50
                  repeat: false
                  function flush() { if (volSlider.pending >= 0) { if (root.svc) root.svc.setMicSetting("volume", volSlider.pending); volSlider.pending = -1 } }
                  onTriggered: flush()
                }
                onMoved: function(v) {
                  volSlider.pending = v
                  if (!volThrottle.running) { volThrottle.flush(); volThrottle.start() }
                }
                onReleased: function(v) { volThrottle.stop(); volSlider.pending = -1; if (root.svc) root.svc.setMicSetting("volume", v) }
                onRightClicked: if (root.svc) root.svc.setMicSetting("volume", 0.5)
              }
            }

            SwitchRow {  // master: the card names what is on without opening anything
              label: "Microphone effects"
              summary: root.m.enabled ? root.effectsSummary() : ""
              strong: true
              checked: !!root.m.enabled
              onToggled: if (root.svc) root.svc.setMicSetting("enabled", !root.m.enabled)
            }
            Column {  // one click to a sound that works; the sections are the "I care" path
              width: parent.width
              spacing: Style.space(4)
              opacity: root.m.enabled ? 1 : 0.55   // off dims the tab; it never blanks it
              Text {
                text: root.activePreset === "custom" ? "Preset · custom" : "Preset"
                color: root.dim
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
              Flow {
                width: parent.width - root.trailInset
                spacing: Style.space(4)
                Repeater {
                  model: root.presetList
                  delegate: Button {
                    required property var modelData
                    text: modelData.label
                    fontSize: Style.font.bodySmall
                    fontFamily: root.fontFamily
                    foreground: root.fg
                    bordered: true
                    // Picking a preset switches the effects on: this row works
                    // with the master off, so you can look before you commit.
                    selected: root.activePreset === modelData.key
                    enabled: !!root.svc
                    onClicked: root.applyPreset(modelData.key)
                  }
                }
              }
            }

            SectionHeader {  // the plumbing: what the microphone needs to sound clean
              label: "Cleanup"
              summary: root.cleanupSummary()
              open: root.cleanupOpen
              dimmed: !root.m.enabled
              onToggled: root.cleanupOpen = !root.cleanupOpen
            }
            Column {
              width: parent.width
              spacing: Style.space(6)
              visible: root.cleanupOpen
              SwitchRow {  // spectral noise suppression: fans, keyboards, the street
                label: "Voice isolation"
                indent: Style.space(10)
                enabled: !!root.svc && !!root.m.enabled
                checked: !!root.m.voiceIsolation
                onToggled: if (root.svc) root.svc.setMicSetting("voiceIsolation", !root.m.voiceIsolation)
              }
              IntensityRow {
                visible: !!root.m.voiceIsolation && !!root.m.enabled
                label: "Strength"
                indent: Style.space(10)
                value: root.m.voiceIsolationIntensity !== undefined ? root.m.voiceIsolationIntensity : 0.6
                onReleased: function(v) { if (root.svc) root.svc.setMicSetting("voiceIsolationIntensity", v) }
              }
              Note {  // it analyses 43 ms at a time; that is what makes it work
                visible: !!root.m.voiceIsolation && !!root.m.enabled
                text: "Adds about 40 ms of delay while it is on."
                x: Style.space(10)
                width: parent.width - root.trailInset - Style.space(10)
              }
              SwitchRow {  // silence between sentences
                label: "Noise gate"
                indent: Style.space(10)
                enabled: !!root.svc && !!root.m.enabled
                checked: !!root.m.noiseGate
                onToggled: if (root.svc) root.svc.setMicSetting("noiseGate", !root.m.noiseGate)
              }
              IntensityRow {
                visible: !!root.m.noiseGate && !!root.m.enabled
                label: "Strength"
                indent: Style.space(10)
                value: root.m.noiseGateIntensity !== undefined ? root.m.noiseGateIntensity : 0.4
                onReleased: function(v) { if (root.svc) root.svc.setMicSetting("noiseGateIntensity", v) }
              }
              SwitchRow {  // compressor + makeup: near and far sound alike
                label: "Auto level"
                indent: Style.space(10)
                enabled: !!root.svc && !!root.m.enabled
                checked: !!root.m.autoLevel
                onToggled: if (root.svc) root.svc.setMicSetting("autoLevel", !root.m.autoLevel)
              }
              IntensityRow {
                visible: !!root.m.autoLevel && !!root.m.enabled
                label: "Strength"
                indent: Style.space(10)
                value: root.m.autoLevelIntensity !== undefined ? root.m.autoLevelIntensity : 0.6
                onReleased: function(v) { if (root.svc) root.svc.setMicSetting("autoLevelIntensity", v) }
              }
              SwitchRow {  // sibilance: turns down "s" and "t" without dulling the voice
                label: "De-esser"
                indent: Style.space(10)
                enabled: !!root.svc && !!root.m.enabled
                checked: !!root.m.deEsser
                onToggled: if (root.svc) root.svc.setMicSetting("deEsser", !root.m.deEsser)
              }
              IntensityRow {
                visible: !!root.m.deEsser && !!root.m.enabled
                label: "Strength"
                indent: Style.space(10)
                value: root.m.deEsserIntensity !== undefined ? root.m.deEsserIntensity : 0.5
                onReleased: function(v) { if (root.svc) root.svc.setMicSetting("deEsserIntensity", v) }
              }
              SwitchRow {
                label: "Rumble filter"
                indent: Style.space(10)
                enabled: !!root.svc && !!root.m.enabled
                checked: !!root.m.highPass
                onToggled: if (root.svc) root.svc.setMicSetting("highPass", !root.m.highPass)
              }
              SwitchRow {  // 50/60 Hz mains buzz from a ground loop or a nearby transformer
                label: "Hum filter"
                indent: Style.space(10)
                enabled: !!root.svc && !!root.m.enabled
                checked: !!root.m.humFilter
                onToggled: if (root.svc) root.svc.setMicSetting("humFilter", !root.m.humFilter)
              }
            }

            SectionHeader {  // the character: everything you can hear yourself doing
              label: "Character"
              summary: root.characterSummary()
              open: root.characterOpen
              dimmed: !root.m.enabled
              onToggled: root.characterOpen = !root.characterOpen
            }
            Column {
              width: parent.width
              spacing: Style.space(6)
              visible: root.characterOpen
              ChipGrid {
                label: "Tone"
                options: root.toneOpts
                value: root.m.tone || "none"
                enabled: !!root.svc && !!root.m.enabled
                onPicked: function(v) { if (root.svc) root.svc.setMicSetting("tone", v) }
              }
              ChipGrid {
                label: "Voice"
                options: root.voiceOpts
                value: root.m.voice || "none"
                enabled: !!root.svc && !!root.m.enabled
                onPicked: function(v) { if (root.svc) root.svc.setMicSetting("voice", v) }
              }
              ChipGrid {
                label: "Space"
                options: root.spaceOpts
                value: root.m.space || "none"
                enabled: !!root.svc && !!root.m.enabled
                onPicked: function(v) { if (root.svc) root.svc.setMicSetting("space", v) }
              }
            }

            PanelSeparator { foreground: root.fg }

            SwitchRow {
              // Disabled while disconnected: the switch shows what the daemon
              // read from the WirePlumber fragment, and with no daemon it would
              // read "off" for microphones that are in fact hidden — one click
              // from there hides them harder instead of bringing them back.
              label: root.multiMic ? "Hide all raw microphones from apps" : "Hide raw microphone from apps"
              checked: !!root.svc && root.svc.micHideAll
              enabled: !!root.svc && root.connected && !root.svc.setupBusy
              onToggled: if (root.svc) root.svc.runMicHide("all", !root.svc.micHideAll)
            }
            Repeater {  // per-microphone switches, when there is more than one
              model: root.multiMic && root.svc && !root.svc.micHideAll ? root.micList : []
              delegate: SwitchRow {
                required property var modelData
                label: "Hide " + root.micName(modelData)
                indent: Style.space(16)
                checked: !!modelData.hidden
                enabled: !!root.svc && root.connected && !root.svc.setupBusy
                onToggled: if (root.svc) root.svc.runMicHide("mic", modelData.name, !modelData.hidden)
              }
            }
            Note {
              text: "Hiding restarts audio and makes “" + (root.svc ? root.svc.micLabel : "Microphone Effects") + "” your default microphone."
              wrapMode: Text.WordWrap
              width: parent.width - root.trailInset
            }
            PanelSeparator { foreground: root.fg }

            Item {  // every microphone effect back to the built-in defaults
              width: parent.width
              height: root.rowH
              Button {
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                text: "Reset effects"
                fontSize: Style.font.bodySmall
                foreground: root.fg
                fontFamily: root.fontFamily
                bordered: true
                enabled: !!root.svc
                opacity: enabled ? 1 : 0.5
                onClicked: root.micResetConfirm = true
              }
            }
          }

          // ---------- Privacy: block camera · hide the raw cameras ----------
          Column {
            id: privacyCol
            width: body.width
            visible: root.tab === "privacy"
            spacing: Style.space(6)

            SwitchRow {  // privacy shutter: the webcam stays closed, apps get a placeholder
              label: "Block camera"
              checked: root.blocked
              onToggled: if (root.svc) root.svc.setSetting("block", !root.blocked)
            }
            Item {  // what apps see while blocked: kind dropdown + file (own row, indented)
              visible: root.blocked
              width: parent.width
              height: root.rowH
              Note {
                visible: root.blockKind !== "card" && !root.pickerMissing
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.right: blockChoose.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideMiddle
                font.pixelSize: Style.font.bodySmall
                color: root.blockSourceNow && root.blockKindPick === "" ? root.fg : root.dim
                text: root.blockSourceNow && root.blockKindPick === "" ? root.basename(root.blockSourceNow) : "No file chosen"
              }
              TextField {  // fallback when there is no zenity to pick with
                visible: root.blockKind !== "card" && root.pickerMissing
                anchors.left: parent.left
                anchors.leftMargin: Style.space(20)
                anchors.right: blockChoose.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                foreground: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                placeholderText: root.blockKind === "image" ? "/path/to/image.png" : "/path/to/video.mp4"
                text: root.blockSourceNow
                onEditingFinished: if (root.svc && text !== "") root.svc.setSetting("blockSource", text)
              }
              Button {
                id: blockChoose
                visible: root.blockKind !== "card"
                anchors.right: blockKindDropdown.left
                anchors.rightMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
                text: "Choose…"
                fontSize: Style.font.bodySmall
                foreground: root.fg
                fontFamily: root.fontFamily
                bordered: true
                enabled: !root.pickerMissing
                opacity: enabled ? 1 : 0.5
                onClicked: pickProc.begin("blockSource", root.blockKind === "image" ? "Placeholder image" : "Placeholder video",
                                          root.blockKind === "image" ? root.imageFilter : root.videoFilter)
              }
              Dropdown {
                id: blockKindDropdown
                anchors.right: parent.right
                anchors.rightMargin: root.trailInset
                anchors.verticalCenter: parent.verticalCenter
                width: Style.space(132)
                showLabel: false
                fontFamily: root.fontFamily
                options: [ { value: "card", label: "Camera-off card" }, { value: "image", label: "Image" }, { value: "video", label: "Video" } ]
                value: root.blockKind
                onChanged: function(v) {
                  if (!root.svc) return
                  if (v === "card") { root.blockKindPick = ""; root.svc.setSetting("blockSource", "") }
                  else if (v !== root.blockKindOf(root.blockSourceNow)) root.blockKindPick = v
                  else root.blockKindPick = ""
                }
              }
            }
            SwitchRow {
              // Disabled while disconnected: the switch shows what the daemon
              // read from the system, and with no daemon it reads "off" for
              // devices that are in fact hidden — one click from there hides
              // them harder instead of bringing them back.
              label: root.multiCam ? "Hide all raw cameras from apps" : "Hide raw camera from apps"
              checked: root.svc ? root.svc.hideRaw : false
              enabled: !!root.svc && root.connected && !root.svc.setupBusy
              onToggled: if (root.svc) root.svc.runSetup("hide-all", !root.svc.hideRaw)
            }
            Repeater {  // per-camera switches (USB cameras only), when there is more than one
              model: root.multiCam && root.svc && !root.svc.hideRaw ? root.hideableCams : []
              delegate: SwitchRow {
                required property var modelData
                label: "Hide " + root.shortName(modelData)
                indent: Style.space(16)
                checked: !!modelData.hidden
                enabled: !!root.svc && root.connected && !root.svc.setupBusy
                onToggled: if (root.svc) root.svc.runSetup("hide-camera", modelData.key, !modelData.hidden)
              }
            }

          }
        }
      }

      Item {
        id: footerBlock
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: footerNote.visible ? footerNote.implicitHeight + Style.space(12) : 0
        PanelSeparator {  // marks the scroll edge when the list overflows (the bar itself is transient)
          anchors.top: parent.top
          foreground: root.fg
          visible: footerNote.visible && scrollArea.overflows
        }
        Note {
          id: footerNote
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.bottom: parent.bottom
          visible: text !== ""
          text: root.footer()
        }
      }
    }
  }
}