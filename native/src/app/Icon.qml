import QtQuick
import QtQuick.Shapes

// One of the Electron app's stroke icons (Icons.qml has the paths), tinted by `color`, round caps and joins.
Item {
    id: root
    property string name
    property real size: 16
    property color color: Theme.text
    property real strokeWidth: 1.8

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Repeater {
        model: Icons.get(root.name)
        delegate: Shape {
            required property var modelData
            width: 24
            height: 24
            scale: root.size / 24
            transformOrigin: Item.TopLeft
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: modelData.fill ? -1 : root.strokeWidth
                strokeColor: root.color
                fillColor: modelData.fill ? root.color : "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: modelData.d }
            }
        }
    }
}
