import QtQuick

// Live preview of the virtual camera. Loaded only while the panel is open.
// Shows the *processed* feed: the daemon writes a small JPEG of every output
// frame (~15 Hz) to preview.jpg in its runtime dir while a preview is wanted
// (Service.setPreview), which this polls. Not a reader of the loopback
// device, so it works while an app streams from it too (v4l2loopback lets one
// reader negotiate the format at a time).
Item {
  id: root
  property string path: ""   // .../camera-effects/preview.jpg
  // True when the daemon already mirrors the output (settings.mirror). The
  // preview always shows you a mirror view of yourself, so it flips the feed
  // itself only when the feed is not mirrored already.
  property bool mirror: false
  readonly property bool ready: shown.status === Image.Ready && shown.implicitWidth > 0

  // Two images, front and back: the next frame loads into the back one and
  // is only shown once it decoded (a failed load — the file not written yet,
  // or renamed mid-read — keeps the last good frame). No flicker, no tearing.
  property bool frontIsA: true
  readonly property Image shown: frontIsA ? imgA : imgB
  readonly property Image back: frontIsA ? imgB : imgA
  property int tick: 0

  function poll() {
    if (path === "" || back.status === Image.Loading) return
    tick++
    back.source = "file://" + path + "?" + tick   // a new URL each time: the same one would not reload
  }
  function loaded(img) {
    if (img === back && img.status === Image.Ready && img.implicitWidth > 0) frontIsA = (img === imgA)
  }

  Image {
    id: imgA
    anchors.fill: parent
    visible: root.frontIsA
    cache: false
    asynchronous: true
    fillMode: Image.PreserveAspectCrop
    // Mirror like a real mirror, whatever the output setting is.
    transform: Scale { origin.x: imgA.width / 2; xScale: root.mirror ? 1 : -1 }
    onStatusChanged: root.loaded(imgA)
  }
  Image {
    id: imgB
    anchors.fill: parent
    visible: !root.frontIsA
    cache: false
    asynchronous: true
    fillMode: Image.PreserveAspectCrop
    transform: Scale { origin.x: imgB.width / 2; xScale: root.mirror ? 1 : -1 }
    onStatusChanged: root.loaded(imgB)
  }

  // The daemon refreshes the file every 66 ms; poll at the same rate. Runs
  // only while this component exists (the panel is open).
  Timer { interval: 66; repeat: true; running: root.path !== ""; triggeredOnStart: true; onTriggered: root.poll() }
}
