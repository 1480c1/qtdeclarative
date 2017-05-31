import QtQuick 2.9
import QtQuick.Shapes 1.0

Item {
    width: 320
    height: 480

    PathItem {
        enableVendorExtensions: false

        anchors.fill: parent

        VisualPath {
            strokeWidth: 4
            strokeColor: "red"
            fillGradient: PathLinearGradient {
                x1: 20; y1: 20
                x2: 180; y2: 130
                PathGradientStop { position: 0; color: "blue" }
                PathGradientStop { position: 0.2; color: "green" }
                PathGradientStop { position: 0.4; color: "red" }
                PathGradientStop { position: 0.6; color: "yellow" }
                PathGradientStop { position: 1; color: "cyan" }
            }
            strokeStyle: VisualPath.DashLine
            dashPattern: [ 1, 4 ]
            Path {
                startX: 20; startY: 20
                PathLine { x: 180; y: 130 }
                PathLine { x: 20; y: 130 }
                PathLine { x: 20; y: 20 }
            }
        }
    }
}
