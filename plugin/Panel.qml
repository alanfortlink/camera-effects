import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Bar icon + popup panel for the Omarchy Camera (the macOS "Video Effects"
// menu, Omarchy style): live preview, camera picker, one row per effect,
// reactions, and the "hide raw camera" switches. All state lives in the
// camfxd daemon (see Service.qml); this file only renders and forwards.
Panel {
  id: root
  moduleName: "tank.camera"
  ipcTarget: "tank.camera"

  readonly property var svc: bar && bar.shell ? (bar.shell.serviceFor("tank.camera") || bar.shell.ensureService("tank.camera")) : null
  readonly property bool inUse: svc ? svc.running : false
  readonly property bool connected: svc ? svc.connected : false
  readonly property var s: svc ? svc.settings : ({})
  readonly property var cams: svc ? svc.cameras : []
  readonly property bool multiCam: cams.length > 1
  readonly property bool alwaysShow: setting("alwaysShow", true)

  // The panel's own preview is itself a consumer of the virtual camera, so
  // while it is loaded the daemon reports consumers >= 1 and running = true
  // even when no app is watching. Assumption: the preview counts as exactly
  // one consumer; everything above that is a real app. (The daemon does not
  // tag consumers, so this is the best the UI can do on its own.)
  readonly property bool previewActive: previewLoader.active
  readonly property int appCount: svc ? Math.max(0, svc.consumers - (previewActive ? 1 : 0)) : 0
  readonly property bool appsConnected: appCount > 0

  readonly property color fg: bar ? bar.foreground : Color.foreground
  readonly property color dim: Qt.darker(fg, 1.45)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property int rowH: Style.spacing.controlHeight
  // ToggleSwitch reserves `cursorPad` around its track for the hover ring, so
  // its visible edge sits this far inside the row. Every other trailing
  // control (dropdown, slider, field) gets the same inset so they line up.
  readonly property int trailInset: Style.space(6)
  property bool pickerMissing: false

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
  }

  visible: alwaysShow || inUse
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

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
    if (!connected) return "Starting…"
    if (svc.deviceMissing) return "Needs setup"
    if (svc.error) return svc.error
    var cam = camName(svc.camera)
    var fps = inUse && svc.fps ? " · " + svc.fps + " FPS" : ""
    if (appsConnected) return "In use · " + cam + fps
    if (previewActive && inUse) return "Preview · " + cam + fps
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
    var use = appCount > 0 ? appCount + " app" + (appCount > 1 ? "s" : "") + " connected"
            : previewActive ? "Preview only · camera on while open" : "Idle"
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
    tooltipText: (root.appsConnected ? "Camera in use — effects" : "Camera effects") + " · right-click: Portrait"
    onPressed: function(b) {
      if (b === Qt.RightButton && root.svc) root.svc.setSetting("portrait", !root.s.portrait)
      else root.toggle()
    }
  }

  onOpenedChanged: if (opened && svc) { svc.refresh(); svc.rescan() }

  // Native file chooser for the background image; falls back to a plain path
  // field when zenity is not around.
  Process {
    id: pickProc
    property string picked: ""
    property int exitCode: -1
    property bool streamDone: false
    command: ["sh", "-c",
      'command -v zenity >/dev/null 2>&1 || exit 127; ' +
      'exec zenity --file-selection --title="Background image" ' +
      '--file-filter="Images | *.png *.jpg *.jpeg *.webp *.bmp" --file-filter="All files | *"']
    function begin() {
      if (running) return
      picked = ""; exitCode = -1; streamDone = false
      running = true
    }
    function finish() {
      if (exitCode < 0 || !streamDone) return
      if (exitCode === 127) root.pickerMissing = true
      else if (exitCode === 0 && picked !== "" && root.svc) root.svc.setSetting("backgroundImage", picked)
    }
    stdout: StdioCollector { onStreamFinished: { pickProc.picked = String(text).trim(); pickProc.streamDone = true; pickProc.finish() } }
    onExited: function(code) { pickProc.exitCode = code; pickProc.finish() }
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

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(380))
    // No cap (like network/bluetooth): the screen governs. The status footer
    // is pinned under the ScrollView so it never scrolls or clips away.
    contentHeight: panel.fittedContentHeight(column.implicitHeight + footerBlock.height)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      ScrollView {
        id: scrollArea
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footerBlock.top
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: column.implicitHeight > height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff

        Column {
          id: column
          width: scrollArea.availableWidth
          spacing: Style.space(6)

          // ---------- Hero ----------
          PanelHero {
            width: parent.width
            title: root.svc ? root.svc.loopbackLabel : "Omarchy Camera"
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

          // ---------- Not installed / needs setup ----------
          Row {
            width: parent.width
            spacing: Style.space(8)
            visible: root.svc && !root.svc.setupBusy && (!root.svc.installed || root.svc.deviceMissing)
            Button {
              text: root.svc && !root.svc.installed ? "Install (build daemon)" : "Set up virtual camera"
              foreground: root.fg
              fontFamily: root.fontFamily
              bordered: true
              onClicked: if (root.svc) root.svc.installed ? root.svc.runSetup("install") : root.svc.install()
            }
          }
          Text {
            width: parent.width
            visible: root.svc && root.svc.setupOutput !== ""
            wrapMode: Text.WordWrap
            text: root.svc ? root.svc.setupOutput : ""
            color: root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
          }

          // ---------- Preview ----------
          Rectangle {
            id: previewBox
            width: parent.width
            height: Math.round(width * 9 / 16)
            radius: Style.cornerRadius
            color: "#000000"
            clip: true
            visible: root.connected && root.svc && root.svc.loopback !== ""

            Loader {
              id: previewLoader
              anchors.fill: parent
              // Only hold the camera open while the panel is on screen; the
              // preview is itself a consumer of the virtual camera, which is
              // what wakes the daemon so you can see the effects live.
              active: root.opened && previewBox.visible
              source: Qt.resolvedUrl("Preview.qml")
              onLoaded: {
                item.deviceLabel = root.svc.loopbackLabel
                item.devicePath = root.svc.loopback
                // Keep the preview a mirror view of yourself whatever the
                // output setting is: undo the flip only when the feed
                // itself is already mirrored.
                item.mirror = Qt.binding(function() { return !!root.s.mirror })
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
          }
          Note {
            visible: previewBox.visible && root.previewActive && !!root.s.mirror
            text: "Apps get the mirrored feed"
          }

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

          Item { width: parent.width; height: Style.space(6) }
          PanelSeparator { foreground: root.fg }
          Item { width: parent.width; height: Style.space(4) }

          // ---------- Effects ----------
          PanelSectionHeader {
            text: root.multiCam && root.svc && !root.svc.sameForAll ? "EFFECTS · " + root.shortName(root.svc.camera).toUpperCase() : "EFFECTS"
            foreground: root.fg
            fontFamily: root.fontFamily
          }
          SwitchRow { label: "Center Stage"; checked: !!root.s.centerStage; onToggled: if (root.svc) root.svc.setSetting("centerStage", !root.s.centerStage) }
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
                text: root.s.backgroundColor || ""
                onEditingFinished: if (root.svc && valid) root.svc.setSetting("backgroundColor", text)
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
              onClicked: pickProc.begin()
            }
          }
          SwitchRow {  // switch = hand-gesture triggers; the strip below plays one now
            label: "Reactions"
            checked: !!root.s.reactions
            onToggled: if (root.svc) root.svc.setSetting("reactions", !root.s.reactions)
          }
          Column {
            width: parent.width
            spacing: Style.space(2)
            Row {
              x: Style.space(20)
              spacing: Style.space(4)
              Repeater {
                model: root.reactionList
                delegate: Item {
                  required property string modelData
                  width: Style.space(20); height: Style.space(20)
                  Text { anchors.centerIn: parent; text: root.glyph(modelData); font.pixelSize: Style.space(13) }
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
              text: "Click to play · switch enables hand gestures"
            }
          }
          SwitchRow { label: "Mirror"; checked: !!root.s.mirror; onToggled: if (root.svc) root.svc.setSetting("mirror", !root.s.mirror) }

          Item { width: parent.width; height: Style.space(6) }
          PanelSeparator { foreground: root.fg }
          Item { width: parent.width; height: Style.space(4) }

          // ---------- Privacy ----------
          PanelSectionHeader { text: "PRIVACY"; foreground: root.fg; fontFamily: root.fontFamily }
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

      // ---------- Footer ---------- (pinned below the scroll area, never clipped)
      Item {
        id: footerBlock
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: footerNote.visible ? footerNote.implicitHeight + Style.space(12) : 0
        PanelSeparator {  // marks the scroll edge when the list overflows (the bar itself is transient)
          anchors.top: parent.top
          foreground: root.fg
          visible: footerNote.visible && column.implicitHeight > scrollArea.height + 1  // +1: rounding slack
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
