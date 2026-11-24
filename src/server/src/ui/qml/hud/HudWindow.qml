pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import Vicinae

Window {
    id: root

    readonly property bool dictating: Hud.dictation !== null
    readonly property bool hasActivity: Hud.activity !== null
    readonly property bool busy: dictating || hasActivity
    readonly property bool dictationMessage: dictating && Hud.dictation.message !== ""
    readonly property bool activityMessage: hasActivity && Hud.activity.message !== ""
    readonly property bool showsMessage: dictationMessage || activityMessage
    readonly property bool working: (dictating && !dictationMessage && (Hud.dictation.transcribing || Hud.dictation.rewriting)) || (hasActivity && !activityMessage)
    readonly property bool recording: dictating && !dictationMessage && !working
    readonly property bool captureReady: recording && Hud.dictation.captureReady
    readonly property color mutedColor: Config.withAlpha(Theme.foreground, 0.6)

    // The dictation HUD scales as a whole, notices keep their size.
    readonly property real s: dictating ? Math.max(1, Hud.scale) : 1
    readonly property bool live: dictating && Hud.dictation.livePreview
    readonly property real barHeight: 42 * s
    readonly property int liveLines: 3
    readonly property real liveTextHeight: liveMetrics.height * liveLines
    readonly property real livePadding: 12 * s
    readonly property real liveAreaHeight: live ? liveTextHeight + livePadding : 0
    // a large scale never makes the HUD wider than the screen it is on
    readonly property real maxWidth: screen && screen.width > 0 ? screen.width - 48 : Number.MAX_VALUE
    readonly property real liveWidth: Math.min(520 * s, maxWidth)
    // Wide enough for every dictation state, so the window never resizes while the pill animates.
    readonly property real dictationWindowWidth: Math.min(live ? liveWidth : 360 * s, maxWidth)

    property color pillColor: Config.withAlpha(Theme.background, busy ? 0.98 : 0.9)
    property color pillBorderColor: Config.withAlpha(Theme.foreground, Theme.isDark ? 0.12 : 0.18)
    property int pillBorderWidth: 1
    property real pillRadius: live ? 16 * s : height / 2
    property real pillWidth: busy ? Math.min(maxWidth, live ? liveWidth : showsMessage ? Math.max(216 * s, Math.min(360 * s, messageLabel.implicitWidth + 72 * s)) : working ? Math.max(216 * s, Math.min(360 * s, workLabel.implicitWidth + 88 * s)) : 172 * s) : noticeRow.width + 30

    signal shown

    width: pillWidth
    height: busy ? barHeight + liveAreaHeight : noticeRow.height + 20
    title: dictating ? qsTr("Dictation") : hasActivity ? Hud.activity.text : Hud.text
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.ToolTip | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    visible: false

    Behavior on pillWidth {
        enabled: root.visible && root.busy && !root.showsMessage && !root.live
        NumberAnimation {
            duration: 320
            easing.type: Easing.OutCubic
        }
    }

    onVisibleChanged: {
        if (visible) {
            appear.restart();
            shown();
        }
    }

    NumberAnimation {
        id: appear
        target: pill
        property: "opacity"
        from: 0
        to: 1
        duration: 180
        easing.type: Easing.OutQuad
    }

    Component.onCompleted: Hud.registerWindow(root)

    FontMetrics {
        id: liveMetrics
        font.family: Theme.fontFamily
        font.pointSize: Theme.regularFontSize * root.s
    }

    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    component HudButton: Item {
        id: button
        property var icon
        required property string label
        property bool status: false
        readonly property bool hovered: mouse.containsMouse
        signal clicked

        implicitWidth: 28 * root.s
        implicitHeight: 28 * root.s
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.onPressAction: if (enabled)
            clicked()

        Rectangle {
            anchors.centerIn: parent
            width: 22 * root.s
            height: 22 * root.s
            radius: width / 2
            color: Config.withAlpha(button.hovered ? Theme.accent : Theme.foreground, button.hovered ? 0.18 : 0.08)
            opacity: button.status && !button.hovered ? 0 : 1
            scale: mouse.pressed ? 0.92 : button.hovered ? 1.05 : 1

            Behavior on color {
                ColorAnimation {
                    duration: 150
                }
            }
            Behavior on opacity {
                NumberAnimation {
                    duration: 150
                }
            }
            Behavior on scale {
                NumberAnimation {
                    duration: 100
                }
            }
        }

        ViciImage {
            anchors.centerIn: parent
            visible: !button.status
            width: 11 * root.s
            height: 11 * root.s
            source: button.icon
            sourceSize: Qt.size(Math.ceil(11 * root.s), Math.ceil(11 * root.s))
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }

        ViciToolTip {
            text: button.label
            visible: button.hovered
        }
    }

    Rectangle {
        id: pill
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.pillWidth
        height: parent.height
        radius: root.pillRadius
        color: root.pillColor
        border.color: root.pillBorderColor
        border.width: root.pillBorderWidth

        RowLayout {
            id: noticeRow
            visible: !root.busy
            anchors.centerIn: parent
            spacing: 5

            ViciImage {
                visible: Hud.hasIcon
                source: Hud.icon
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                sourceSize: Qt.size(16, 16)
            }

            Text {
                text: Hud.text
                color: Theme.foreground
                font.family: Theme.fontFamily
                font.pointSize: Theme.smallerFontSize
                maximumLineCount: 1
                elide: Text.ElideRight
                Layout.maximumWidth: 270
            }
        }

        Item {
            id: bar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: root.barHeight
            visible: root.busy

            HudButton {
                id: finishButton
                anchors.left: parent.left
                anchors.leftMargin: 6 * root.s
                anchors.verticalCenter: parent.verticalCenter
                visible: root.recording
                enabled: root.dictating && Hud.dictation.showControls
                label: qsTr("Stop and transcribe")
                status: true
                onClicked: if (Hud.dictation)
                    Hud.dictation.accept()

                Rectangle {
                    anchors.centerIn: parent
                    width: 7 * root.s
                    height: 7 * root.s
                    radius: finishButton.hovered ? 2 * root.s : width / 2
                    color: root.captureReady ? Theme.accent : root.mutedColor
                    opacity: root.captureReady ? 1 : 0.45
                    Behavior on radius {
                        NumberAnimation {
                            duration: 120
                        }
                    }

                    Rectangle {
                        id: pulseHalo
                        anchors.centerIn: parent
                        width: 7 * root.s
                        height: 7 * root.s
                        radius: width / 2
                        color: "transparent"
                        border.color: Theme.accent
                        border.width: 1
                        visible: root.captureReady && !finishButton.hovered

                        ParallelAnimation {
                            running: root.visible && root.captureReady && !finishButton.hovered
                            loops: Animation.Infinite
                            NumberAnimation {
                                target: pulseHalo
                                property: "scale"
                                from: 1
                                to: 2.8
                                duration: 1900
                                easing.type: Easing.OutQuad
                            }
                            NumberAnimation {
                                target: pulseHalo
                                property: "opacity"
                                from: 0.35
                                to: 0
                                duration: 1900
                                easing.type: Easing.OutQuad
                            }
                        }
                    }
                }
            }

            ViciSpinner {
                anchors.left: parent.left
                anchors.leftMargin: 14 * root.s
                anchors.verticalCenter: parent.verticalCenter
                visible: root.working
                width: 13 * root.s
                height: 13 * root.s
                color: Theme.accent
            }

            AudioWaveform {
                anchors.centerIn: parent
                visible: root.recording
                active: root.visible && root.recording
                ready: root.captureReady
                level: root.captureReady ? Hud.dictation.audioLevel : 0
                barWidth: Math.round(4 * root.s)
                maxHeight: Math.round(18 * root.s)
                spacing: Math.round(3 * root.s)
            }

            Text {
                id: workLabel
                anchors.centerIn: parent
                width: Math.min(implicitWidth, 272 * root.s)
                visible: root.working
                text: root.hasActivity ? Hud.activity.text : root.dictating && Hud.dictation.rewriting ? qsTr("Rewriting") : qsTr("Transcribing")
                color: root.mutedColor
                font.family: Theme.fontFamily
                font.pointSize: Theme.smallerFontSize * root.s
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            Text {
                id: messageLabel
                anchors.left: parent.left
                anchors.leftMargin: 18 * root.s
                anchors.right: cancelButton.left
                anchors.rightMargin: 6 * root.s
                anchors.verticalCenter: parent.verticalCenter
                visible: root.showsMessage
                text: root.hasActivity ? Hud.activity.message : root.dictating ? Hud.dictation.message : ""
                color: Theme.foreground
                font.family: Theme.fontFamily
                font.pointSize: Theme.smallerFontSize * root.s
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            HudButton {
                id: cancelButton
                anchors.right: parent.right
                anchors.rightMargin: 6 * root.s
                anchors.verticalCenter: parent.verticalCenter
                label: root.showsMessage ? qsTr("Dismiss") : root.hasActivity ? qsTr("Cancel") : qsTr("Cancel dictation")
                icon: Img.icon(BuiltinIcon.Xmark).withFillColor(hovered ? Theme.foreground : root.mutedColor)
                onClicked: {
                    if (Hud.activity)
                        Hud.activity.cancel();
                    else if (Hud.dictation)
                        Hud.dictation.cancel();
                }
            }
        }

        Rectangle {
            anchors.top: bar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16 * root.s
            anchors.rightMargin: 16 * root.s
            height: 1
            visible: root.live
            color: Config.withAlpha(Theme.foreground, 0.08)
        }

        Flickable {
            id: liveArea
            anchors.top: bar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: root.livePadding / 3
            anchors.leftMargin: 18 * root.s
            anchors.rightMargin: 18 * root.s
            height: root.liveTextHeight
            visible: root.live
            clip: true
            interactive: false
            contentWidth: width
            contentHeight: liveText.implicitHeight
            // the newest words stay in view
            contentY: Math.max(0, contentHeight - height)

            Behavior on contentY {
                NumberAnimation {
                    duration: 160
                    easing.type: Easing.OutQuad
                }
            }

            // Words fade in as they are recognized, and tentative (dimmed) words brighten gently when they
            // settle. Each word's opacity lives in its text color, so the layout and wrapping never move.
            Text {
                id: liveText
                readonly property string committed: root.live ? Hud.dictation.liveCommitted : ""
                readonly property string tentative: root.live ? Hud.dictation.liveTentative : ""
                readonly property bool empty: committed === "" && tentative === ""
                // long enough to read as a soft arrival rather than a blink
                readonly property int fadeMs: 550
                readonly property real tentativeLevel: 0.6
                // {text, settled, born, settledAt}; times in ms since the epoch
                property var words: []
                property string rendered: ""
                property bool animating: false

                width: liveArea.width
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                color: Theme.foreground
                font: liveMetrics.font
                text: empty ? "<font color=\"" + root.mutedColor + "\">" + (root.working ? qsTr("Transcribing…") : qsTr("Listening…")) + "</font>" : rendered
                Accessible.role: Accessible.StaticText
                Accessible.name: committed + " " + tentative

                onCommittedChanged: Qt.callLater(syncWords)
                onTentativeChanged: Qt.callLater(syncWords)

                function splitWords(text, settled) {
                    return text.split(/\s+/).filter(w => w.length > 0).map(w => ({
                                text: w,
                                settled: settled
                            }));
                }

                // A word the model only re-punctuated or re-capitalized is the same word.
                function sameWord(a, b) {
                    const norm = w => w.toLowerCase().replace(/[.,!?;:"“”‘’()\[\]…]/g, "");
                    return a.text === b.text || norm(a.text) === norm(b.text);
                }

                function inherit(next, prev, now) {
                    next.born = prev.born;
                    next.settledAt = next.settled && !prev.settled ? now : prev.settledAt;
                }

                // Matches the new words to the shown ones (shared prefix, then a longest common subsequence),
                // so only words that are new or were corrected fade in.
                function syncWords() {
                    const now = Date.now();
                    const next = splitWords(committed, true).concat(splitWords(tentative, false));
                    const prev = words;

                    let p = 0;
                    while (p < prev.length && p < next.length && sameWord(prev[p], next[p])) {
                        inherit(next[p], prev[p], now);
                        ++p;
                    }

                    const a = prev.slice(p);
                    const b = next.slice(p);
                    const n = a.length;
                    const m = b.length;
                    if (n > 0 && m > 0 && n * m <= 250000) {
                        const w = m + 1;
                        const dp = new Int32Array((n + 1) * w);
                        for (let i = n - 1; i >= 0; --i) {
                            for (let j = m - 1; j >= 0; --j) {
                                dp[i * w + j] = sameWord(a[i], b[j]) ? dp[(i + 1) * w + j + 1] + 1 : Math.max(dp[(i + 1) * w + j], dp[i * w + j + 1]);
                            }
                        }
                        let i = 0;
                        let j = 0;
                        while (i < n && j < m) {
                            if (sameWord(a[i], b[j])) {
                                inherit(b[j], a[i], now);
                                ++i;
                                ++j;
                            } else if (dp[(i + 1) * w + j] >= dp[i * w + j + 1]) {
                                ++i;
                            } else {
                                ++j;
                            }
                        }
                    }

                    for (const word of next) {
                        if (word.born === undefined) {
                            word.born = now;
                            word.settledAt = word.settled ? now : undefined;
                        }
                    }
                    words = next;
                    render(now);
                }

                // gentle at both ends
                function ease(t) {
                    return t >= 1 ? 1 : (1 - Math.cos(Math.PI * Math.max(0, t))) / 2;
                }

                function render(now) {
                    const fg = Theme.foreground;
                    let html = "";
                    let busy = false;
                    for (let i = 0; i < words.length; ++i) {
                        const word = words[i];
                        const shown = ease((now - word.born) / fadeMs);
                        let level = tentativeLevel;
                        if (word.settled) {
                            const settling = word.settledAt === undefined ? 1 : ease((now - word.settledAt) / fadeMs);
                            level = tentativeLevel + (1 - tentativeLevel) * settling;
                            busy = busy || settling < 1;
                        }
                        busy = busy || shown < 1;
                        const color = Qt.rgba(fg.r, fg.g, fg.b, shown * level);
                        html += (i > 0 ? " " : "") + "<font color=\"" + color + "\">" + root.escapeHtml(word.text) + "</font>";
                    }
                    rendered = html;
                    animating = busy;
                }

                Timer {
                    interval: 16
                    repeat: true
                    running: liveText.animating && root.visible
                    onTriggered: liveText.render(Date.now())
                }
            }
        }
    }
}
