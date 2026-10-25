import QtQuick
import QtQuick.Shapes

// A rounded box with a dashed 1px border (the Electron app's `border border-dashed`), over an optional fill.
Item {
    id: box
    property real radius: 22
    property color borderColor: Theme.lineStrong
    property color fillColor: "transparent"

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 1
            strokeColor: box.borderColor
            fillColor: box.fillColor
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 3]
            PathRectangle {
                x: 0.5
                y: 0.5
                width: box.width - 1
                height: box.height - 1
                radius: box.radius
            }
        }
    }
}
