import QtQuick
import QtQuick.Shapes

// The Electron app's tool-call spinner: a 14px ring with a quarter missing, turning.
Item {
    id: spin
    property color color: Theme.accent
    property real size: 14
    width: size
    height: size

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 2
            strokeColor: spin.color
            fillColor: "transparent"
            capStyle: ShapePath.FlatCap
            PathAngleArc {
                centerX: spin.size / 2
                centerY: spin.size / 2
                radiusX: spin.size / 2 - 1
                radiusY: spin.size / 2 - 1
                startAngle: 0
                sweepAngle: 270
            }
        }
        RotationAnimation on rotation {
            running: spin.visible
            from: 0
            to: 360
            duration: 800
            loops: Animation.Infinite
        }
    }
}
