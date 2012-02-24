import QtQuick 2.0

Rectangle {
    id: root
    width: 500
    height: 600

    // time to pause between each add, remove, etc.
    // (obviously, must be less than 'duration' value to actually test that
    // interrupting transitions will still produce the correct result)
    property int timeBetweenActions: duration / 2

    property int duration: 300

    property int count: list.count

    Component {
        id: myDelegate
        Rectangle {
            id: wrapper
            objectName: "wrapper"
            height: 20
            width: 240
            Text { text: index }
            Text {
                x: 30
                id: textName
                objectName: "textName"
                text: name
            }
            Text {
                x: 200
                text: wrapper.y
            }
            color: ListView.isCurrentItem ? "lightsteelblue" : "white"
        }
    }

    ListView {
        id: list

        property bool populateDone

        property bool runningAddTargets: false
        property bool runningAddDisplaced: false
        property bool runningMoveTargets: false
        property bool runningMoveDisplaced: false

        objectName: "list"
        focus: true
        anchors.centerIn: parent
        width: 240
        height: 320
        model: testModel
        delegate: myDelegate

        add: Transition {
            id: addTargets
            SequentialAnimation {
                ScriptAction { script: list.runningAddTargets = true }
                ParallelAnimation {
                    NumberAnimation { properties: "x"; from: addTargets_transitionFrom.x; duration: root.duration }
                    NumberAnimation { properties: "y"; from: addTargets_transitionFrom.y; duration: root.duration }
                }
                ScriptAction { script: list.runningAddTargets = false }
            }
        }

        addDisplaced: Transition {
            id: addDisplaced
            SequentialAnimation {
                ScriptAction { script: list.runningAddDisplaced = true }
                ParallelAnimation {
                    NumberAnimation { properties: "x"; from: addDisplaced_transitionFrom.x; duration: root.duration }
                    NumberAnimation { properties: "y"; from: addDisplaced_transitionFrom.y; duration: root.duration }
                }
                ScriptAction { script: list.runningAddDisplaced = false }
            }
        }

        move: Transition {
            id: moveTargets
            SequentialAnimation {
                ScriptAction { script: list.runningMoveTargets = true }
                ParallelAnimation {
                    NumberAnimation { properties: "x"; from: moveTargets_transitionFrom.x; duration: root.duration }
                    NumberAnimation { properties: "y"; from: moveTargets_transitionFrom.y; duration: root.duration }
                }
                ScriptAction { script: list.runningMoveTargets = false }
            }
        }

        moveDisplaced: Transition {
            id: moveDisplaced
            SequentialAnimation {
                ScriptAction { script: list.runningMoveDisplaced = true }
                ParallelAnimation {
                    NumberAnimation { properties: "x"; from: moveDisplaced_transitionFrom.x; duration: root.duration }
                    NumberAnimation { properties: "y"; from: moveDisplaced_transitionFrom.y; duration: root.duration }
                }
                ScriptAction { script: list.runningMoveDisplaced = false }
            }
        }
    }

    Rectangle {
        anchors.fill: list
        color: "lightsteelblue"
        opacity: 0.2
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: 20; height: 20
        color: "white"
        NumberAnimation on x { loops: Animation.Infinite; from: 0; to: 300; duration: 100000 }
    }
}



