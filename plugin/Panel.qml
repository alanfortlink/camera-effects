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

  readonly property color fg: bar ? bar.foreground : Color.foreground
  readonly property color dim: Qt.darker(fg, 1.45)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property int rowH: Style.spacing.controlHeight

  visible: alwaysShow || inUse
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  function subtitle() {
    if (!svc) return "SERVICE NOT LOADED"
    if (svc.setupBusy) return "WORKING…"
    if (!svc.installed) return "NOT INSTALLED"
    if (!connected) return "STARTING…"
    if (svc.deviceMissing) return "NEEDS SETUP"
    if (svc.error) return svc.error.toUpperCase()
    var cam = svc.camera && svc.camera.name ? svc.camera.name : "no camera"
    return (inUse ? "IN USE" : "IDLE") + " · " + cam + (inUse && svc.fps ? " · " + svc.fps + " FPS" : "")
  }
  function glyph(name) {
    return ({ hearts: "❤️", thumbsup: "👍", thumbsdown: "👎", balloons: "🎈", confetti: "🎊", fireworks: "🎆", rain: "🌧️", lasers: "🔦" })[name] || name
  }
  function hint(name) {
    return ({ hearts: "Hearts · two-hand heart", thumbsup: "Thumbs up", thumbsdown: "Thumbs down", balloons: "Balloons · peace sign",
              confetti: "Confetti · two peace signs", fireworks: "Fireworks · two thumbs up", rain: "Rain · two thumbs down", lasers: "Lasers · two rock signs" })[name] || name
  }
  function shortName(c) {
    var n = String(c && c.name ? c.name : "camera")
    return n.length > 26 ? n.slice(0, 25) + "…" : n
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰄀"
    active: root.inUse
    useActiveColor: true
    activeColor: Color.accent
    tooltipText: root.inUse ? "Camera in use — effects" : "Camera effects"
    onPressed: function(b) {
      if (b === Qt.RightButton && root.svc) root.svc.setSetting("portrait", !root.s.portrait)
      else root.toggle()
    }
  }

  onOpenedChanged: if (opened && svc) { svc.refresh(); svc.rescan() }

  // One compact settings row: label on the left, switch on the right.
  component SwitchRow: Item {
    id: sw
    property string label: ""
    property bool checked: false
    property bool enabled: true
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
      anchors.rightMargin: Style.space(4)
      minimum: 0.05; maximum: 1; step: 0.05
      value: parent.value
      onReleased: function(v) { parent.released(v) }
    }
  }

  component Caption: Text {
    width: parent ? parent.width : 200
    color: root.dim
    font.family: root.fontFamily
    font.pixelSize: Style.font.caption
    font.bold: true
    font.letterSpacing: 1.2
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(392))
    contentHeight: panel.fittedContentHeight(column.implicitHeight, Style.space(720))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      ScrollView {
        id: scrollArea
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: column.implicitHeight > height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff

        Column {
          id: column
          width: scrollArea.availableWidth
          spacing: Style.space(6)

          // ---------- Hero ----------
          Item {
            width: parent.width
            height: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight) + Style.space(4)
            Text {
              id: heroIcon
              text: "󰄀"
              color: root.inUse ? Color.accent : root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.display
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
            }
            Column {
              id: heroLabels
              anchors.left: heroIcon.right
              anchors.leftMargin: Style.space(12)
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(1)
              Text {
                text: root.svc ? root.svc.loopbackLabel : "Omarchy Camera"
                color: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
                elide: Text.ElideRight
                width: parent.width
              }
              Caption { text: root.subtitle(); elide: Text.ElideRight }
            }
          }

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
              onClicked: root.svc.installed ? root.svc.runSetup("install") : root.svc.install()
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
              onLoaded: { item.deviceLabel = root.svc.loopbackLabel; item.devicePath = root.svc.loopback }
            }
            // Black until the daemon delivers its first live frame.
            Rectangle {
              anchors.fill: parent
              color: "#000000"
              visible: !root.inUse || !previewLoader.item || !previewLoader.item.ready
              Text {
                anchors.centerIn: parent
                text: root.connected && root.svc && root.svc.camera && root.svc.camera.name ? "Starting camera…" : "No camera"
                color: "#666"
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
            }
            Rectangle {  // gesture the daemon currently sees
              anchors.left: parent.left
              anchors.bottom: parent.bottom
              anchors.margins: Style.space(8)
              visible: root.svc && root.svc.gesture !== ""
              color: "#aa000000"
              radius: Style.space(4)
              width: gestureText.implicitWidth + Style.space(12)
              height: gestureText.implicitHeight + Style.space(6)
              Text {
                id: gestureText
                anchors.centerIn: parent
                text: root.svc ? root.glyph(root.svc.gesture) + "  " + root.svc.gesture : ""
                color: "#fff"
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
            }
          }

          // ---------- Camera ----------
          Dropdown {
            width: parent.width
            visible: !!root.svc && (root.multiCam ||
                     (!!root.svc.camera && !!root.svc.camera.bus && !root.cams.some(function(c) { return c.bus === root.svc.camera.bus })))
            showLabel: false
            fontFamily: root.fontFamily
            options: root.cams.map(function(c) { return { value: c.bus, label: c.name } })
            value: root.svc && root.svc.camera && root.svc.camera.bus ? String(root.svc.camera.bus) : ""
            onChanged: function(v) { if (root.svc) root.svc.selectCamera(v) }
          }
          SwitchRow {
            visible: root.multiCam
            label: "Same effects on every camera"
            checked: root.svc ? root.svc.sameForAll : true
            onToggled: root.svc.setSameForAll(!root.svc.sameForAll)
          }

          PanelSeparator { foreground: root.fg }

          // ---------- Effects ----------
          Caption { text: root.multiCam && root.svc && !root.svc.sameForAll ? "EFFECTS · " + root.shortName(root.svc.camera).toUpperCase() : "EFFECTS" }
          SwitchRow { label: "Center Stage"; checked: !!root.s.centerStage; onToggled: root.svc.setSetting("centerStage", !root.s.centerStage) }
          SwitchRow { label: "Portrait"; checked: !!root.s.portrait; onToggled: root.svc.setSetting("portrait", !root.s.portrait) }
          IntensityRow {
            visible: !!root.s.portrait
            value: root.s.portraitIntensity !== undefined ? root.s.portraitIntensity : 0.6
            onReleased: function(v) { root.svc.setSetting("portraitIntensity", v) }
          }
          SwitchRow { label: "Studio Light"; checked: !!root.s.studioLight; onToggled: root.svc.setSetting("studioLight", !root.s.studioLight) }
          IntensityRow {
            visible: !!root.s.studioLight
            value: root.s.studioLightIntensity !== undefined ? root.s.studioLightIntensity : 0.6
            onReleased: function(v) { root.svc.setSetting("studioLightIntensity", v) }
          }
          Item {  // Background: label · dropdown · value field
            width: parent.width
            height: root.rowH
            Text {
              id: bgLabel
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
              text: "Background"
              color: root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
            }
            Row {
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(6)
              TextField {
                id: bgField
                visible: root.s.background === "color" || root.s.background === "image"
                width: Style.space(120)
                foreground: root.fg
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                placeholderText: root.s.background === "color" ? "#1e1e2e" : "/path/to/image"
                text: root.s.background === "color" ? (root.s.backgroundColor || "") : (root.s.backgroundImage || "")
                onEditingFinished: root.svc.setSetting(root.s.background === "color" ? "backgroundColor" : "backgroundImage", text)
              }
              Dropdown {
                width: Style.space(96)
                showLabel: false
                fontFamily: root.fontFamily
                options: [ { value: "none", label: "None" }, { value: "color", label: "Color" }, { value: "image", label: "Image" } ]
                value: root.s.background || "none"
                onChanged: function(v) { root.svc.setSetting("background", v) }
              }
            }
          }
          Item {  // Reactions: switch = gesture triggers; buttons = play now
            width: parent.width
            height: root.rowH
            Text {
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
              text: "Reactions"
              color: root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
            }
            Row {
              anchors.right: reactSwitch.left
              anchors.rightMargin: Style.space(12)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)
              Repeater {
                model: root.svc ? root.svc.reactionNames : []
                delegate: Item {
                  required property string modelData
                  width: Style.space(24); height: Style.space(24)
                  Text { anchors.centerIn: parent; text: root.glyph(modelData); font.pixelSize: Style.space(15) }
                  MouseArea {
                    id: reactArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.svc.react(modelData)
                    cursorShape: Qt.PointingHandCursor
                  }
                  Rectangle { anchors.fill: parent; radius: Style.space(4); color: root.fg; opacity: reactArea.containsMouse ? 0.12 : 0; z: -1 }
                  PanelToolTip { visible: reactArea.containsMouse; text: root.hint(modelData); fontFamily: root.fontFamily }
                }
              }
            }
            ToggleSwitch {
              id: reactSwitch
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              checked: !!root.s.reactions
              foreground: root.fg
              onToggled: root.svc.setSetting("reactions", !root.s.reactions)
            }
          }
          SwitchRow { label: "Mirror"; checked: !!root.s.mirror; onToggled: root.svc.setSetting("mirror", !root.s.mirror) }

          PanelSeparator { foreground: root.fg }

          // ---------- Privacy ----------
          SwitchRow {
            label: root.multiCam ? "Hide all raw cameras from apps" : "Hide raw camera from apps"
            checked: root.svc ? root.svc.hideRaw : false
            enabled: root.svc && !root.svc.setupBusy
            onToggled: root.svc.runSetup("hide-all", !root.svc.hideRaw)
          }
          Repeater {  // per-camera switches (USB cameras only), when there is more than one
            model: root.multiCam && root.svc && !root.svc.hideRaw ? root.cams.filter(function(c) { return !!c.key }) : []
            delegate: SwitchRow {
              required property var modelData
              label: "Hide " + root.shortName(modelData)
              indent: Style.space(16)
              checked: !!modelData.hidden
              enabled: root.svc && !root.svc.setupBusy
              onToggled: root.svc.runSetup("hide-camera", modelData.key, !modelData.hidden)
            }
          }
          Caption {
            font.bold: false
            font.letterSpacing: 0
            elide: Text.ElideRight
            text: root.svc && root.connected
              ? root.svc.loopback + " · " + (root.svc.state.output ? root.svc.state.output.width + "×" + root.svc.state.output.height : "") +
                (root.svc.consumers ? " · " + root.svc.consumers + " app" + (root.svc.consumers > 1 ? "s" : "") + " connected" : " · idle")
              : ""
          }
        }
      }
    }
  }
}
