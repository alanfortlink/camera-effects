import QtQuick
import QtMultimedia

// Live preview of the virtual camera. Loaded only while the panel is open so
// the shell holds the camera for as short a time as possible. Reads the
// *processed* feed (the loopback device), so what you see is what apps get.
Item {
  id: root
  property string deviceLabel: "Iris Camera"
  property string devicePath: ""
  // True when the daemon already mirrors the output (settings.mirror). The
  // preview always shows you a mirror view of yourself, so it flips the feed
  // itself only when the feed is not mirrored already.
  property bool mirror: false
  readonly property bool ready: cam.active && vo.videoSink && vo.videoSink.videoSize.width > 0

  MediaDevices { id: devs }

  function pickDevice() {
    var list = devs.videoInputs
    for (var i = 0; i < list.length; i++) if (list[i].description === deviceLabel) return list[i]
    for (var j = 0; j < list.length; j++) if (String(list[j].id) === devicePath) return list[j]
    return null
  }

  function start() {
    var d = pickDevice()
    if (!d) { retry.start(); return }
    cam.cameraDevice = d
    cam.start()
  }

  Camera {
    id: cam
    onErrorOccurred: function(e, msg) { console.warn("camera preview:", msg); retry.start() }
  }
  CaptureSession { camera: cam; videoOutput: vo }
  VideoOutput {
    id: vo
    anchors.fill: parent
    fillMode: VideoOutput.PreserveAspectCrop
    // Mirror like a real mirror, whatever the output setting is.
    transform: Scale { origin.x: vo.width / 2; xScale: root.mirror ? 1 : -1 }
  }

  // The loopback only shows up in Qt's device list once the daemon has
  // primed it; poll briefly if it isn't there yet.
  Timer { id: retry; interval: 700; repeat: false; onTriggered: root.start() }
  Connections { target: devs; function onVideoInputsChanged() { if (!cam.active) root.start() } }

  Component.onCompleted: start()
  Component.onDestruction: cam.stop()
}
