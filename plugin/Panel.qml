import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Bar icon + popup panel for the Camera Effects (the macOS "Video Effects"
// menu, Omarchy style): live preview (drag to pan, wheel to zoom), camera
// picker, one row per effect (switches, the Zoom slider, Fit/Filter/Effects/
// Rotate dropdowns), reactions, a snapshot button, and the privacy switches
// (block camera, hide raw camera). All state lives in the camera-effects-server daemon (see
// Service.qml); this file only renders and forwards.
Panel {
  id: root
  moduleName: "alanfortlink.camera-effects"
  ipcTarget: "alanfortlink.camera-effects"
  // manageIpc: false so this panel can own the single IpcHandler the target
  // permits — needed for `snap` below.
  manageIpc: false

  // The shell mounts our service at startup (kinds: service); calling
  // ensureService() from a binding would mutate the registry it reads (loop).
  readonly property var svc: bar && bar.shell ? bar.shell.serviceFor("alanfortlink.camera-effects") : null
  readonly property bool inUse: svc ? svc.running : false
  readonly property bool connected: svc ? svc.connected : false
  readonly property var s: svc ? svc.settings : ({})
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
  Component.onCompleted: zoomLive = zoomNow

  // The preview is served by the daemon (a JPEG it writes while we ask for
  // it, see Preview.qml), not read from the virtual camera: consumers are
  // all real apps. While the panel is open the daemon runs the pipeline for
  // the preview even with no app watching (svc.previewOn).
  readonly property bool previewActive: previewLoader.active
  onPreviewActiveChanged: if (svc) svc.setPreview(previewActive)
  Component.onDestruction: if (svc && previewActive) svc.setPreview(false)
  readonly property int appCount: svc ? svc.consumers : 0
  readonly property bool appsConnected: appCount > 0
  readonly property bool previewOnly: !!svc && svc.previewOn && !appsConnected

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

  visible: alwaysShow || inUse
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
    function close() { root.close() }
    function show() { root.open() }
    function hide() { root.close() }
    function toggle() { root.toggle() }
    function snap() { root.snap() }   // omarchy-shell alanfortlink.camera-effects snap
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
  function footer() {
    if (!svc || !connected) return ""
    var res = svc.state.output ? " · " + svc.state.output.width + "×" + svc.state.output.height : ""
    // Keep it under ~50 monospace caption characters so it never elides.
    var apps = svc.consumerApps || []
    var use = appCount > 0 ? "In use by " + (apps.length ? apps.join(", ") : appCount + " app" + (appCount > 1 ? "s" : ""))
            : previewOnly && inUse ? (blocked ? "Preview only · camera blocked" : "Preview only · camera on while open") : "Idle"
    return use + res
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰄀"
    active: root.appsConnected
    useActiveColor: true
    activeColor: Color.accent
    tooltipText: (root.appsConnected ? (root.blocked ? "Camera blocked — apps get the placeholder" : "Camera in use — effects")
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
    signal toggled()
    width: parent ? parent.width : 200
    height: root.rowH
    opacity: enabled ? 1 : 0.5
    Text {
      anchors.left: parent.left
      anchors.leftMargin: sw.indent
      anchors.right: swToggle.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      text: sw.label
      color: root.fg
      font.family: root.fontFamily
      font.pixelSize: Style.font.body
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

  component IntensityRow: Item {
    property real value: 0.6
    signal released(real v)
    width: parent ? parent.width : 200
    height: root.rowH * 0.8
    PanelSlider {
      bar: root.bar
      anchors.fill: parent
      anchors.leftMargin: Style.space(20)
      anchors.rightMargin: root.trailInset
      minimum: 0.05; maximum: 1; step: 0.05
      value: parent.value
      onReleased: function(v) { parent.released(v) }
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
    { value: "tophat", label: "Top hat" }, { value: "crown", label: "Crown" }, { value: "cat", label: "Cat" }, { value: "halo", label: "Halo" },
    { value: "headphones", label: "Headphones" }, { value: "flowers", label: "Flowers" } ]
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
  // what the preview must leave room for (see previewBox.roomH).
  readonly property real restH: topBlock.implicitHeight + Style.space(6) + previewNote.blockH + Style.space(10)
                                + body.implicitHeight + footerBlock.height

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    // Hero and preview stay put at the top; below them two columns (camera
    // and framing | video effects and privacy) scroll when the screen is
    // short. The screen governs both axes (no cap, like network/bluetooth);
    // the status footer is pinned under the ScrollView so it never scrolls
    // or clips away.
    contentWidth: panel.fittedContentWidth(Style.space(700))
    contentHeight: panel.fittedContentHeight(root.restH + previewRow.height)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

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
          visible: root.svc && (root.svc.setupOutput !== "" || (root.needsRebuild && root.svc.daemonLog !== ""))
          wrapMode: Text.WordWrap
          text: root.svc ? (root.svc.setupOutput !== "" ? root.svc.setupOutput
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
          visible: root.connected && root.svc && root.svc.loopback !== ""

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
            Text {
              anchors.centerIn: parent
              text: root.connected && root.svc && root.svc.camera && root.svc.camera.name ? "Starting camera…" : "No camera"
              color: Qt.rgba(1, 1, 1, 0.5)  // the box is always black
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
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

      // ---------- Scrolling body: two columns ----------
      ScrollView {
        id: scrollArea
        anchors.top: previewNote.bottom
        anchors.topMargin: Style.space(10)
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
          readonly property int gap: Style.space(16)
          width: scrollArea.availableWidth
          implicitWidth: width
          implicitHeight: Math.max(leftCol.implicitHeight, rightCol.implicitHeight)

          // ---------- Left: camera · framing ----------
          Column {
            id: leftCol
            x: 0
            width: Math.round((body.width - body.gap) * 0.48)
            spacing: Style.space(6)

            PanelSectionHeader { text: "CAMERA"; foreground: root.fg; fontFamily: root.fontFamily }
            // ---------- Camera ----------
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
                onReleased: function(v) { root.setFraming({ zoom: Math.round(v * 10) / 10 }) }
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
          }

          PanelSeparator {  // thin rule between the columns
            x: leftCol.width + Math.floor((body.gap - width) / 2)
            width: 1
            height: body.implicitHeight
            foreground: root.fg
          }

          // ---------- Right: video effects · privacy ----------
          Column {
            id: rightCol
            x: leftCol.width + body.gap
            width: body.width - x
            spacing: Style.space(6)

            // ---------- Video ----------
            PanelSectionHeader {
              text: root.multiCam && root.svc && !root.svc.sameForAll ? "VIDEO · " + root.shortName(root.svc.camera).toUpperCase() : "VIDEO"
              foreground: root.fg
              fontFamily: root.fontFamily
            }
            SwitchRow { label: "Center Stage"; checked: !!root.s.centerStage; onToggled: if (root.svc) root.svc.setSetting("centerStage", !root.s.centerStage) }
            IntensityRow {  // how tight / how eagerly it follows: left = calm and wide, right = tight and snappy
              visible: !!root.s.centerStage
              value: root.s.centerStageIntensity !== undefined ? root.s.centerStageIntensity : 0.5
              onReleased: function(v) { if (root.svc) root.svc.setSetting("centerStageIntensity", v) }
            }
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
                  options: [ { value: "none", label: "None" }, { value: "color", label: "Color" }, { value: "image", label: "Image" } ]
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
            LabelDropdownRow { label: "Filter"; options: root.filterOpts; value: root.s.filter || "none"; onChanged: function(v) { if (root.svc) root.svc.setSetting("filter", v) } }
            LabelDropdownRow { label: "Effects"; options: root.funOpts; value: root.s.fun || "none"; onChanged: function(v) { if (root.svc) root.svc.setSetting("fun", v) } }
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

            Item { width: parent.width; height: Style.space(6) }
            PanelSeparator { foreground: root.fg }
            Item { width: parent.width; height: Style.space(4) }

            // ---------- Privacy ----------
            PanelSectionHeader { text: "PRIVACY"; foreground: root.fg; fontFamily: root.fontFamily }
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
              label: root.multiCam ? "Hide all raw cameras from apps" : "Hide raw camera from apps"
              checked: root.svc ? root.svc.hideRaw : false
              enabled: !!root.svc && !root.svc.setupBusy
              onToggled: if (root.svc) root.svc.runSetup("hide-all", !root.svc.hideRaw)
            }
            Repeater {  // per-camera switches (USB cameras only), when there is more than one
              model: root.multiCam && root.svc && !root.svc.hideRaw ? root.hideableCams : []
              delegate: SwitchRow {
                required property var modelData
                label: "Hide " + root.shortName(modelData)
                indent: Style.space(16)
                checked: !!modelData.hidden
                enabled: !!root.svc && !root.svc.setupBusy
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
