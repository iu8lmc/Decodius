import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Effects
import Decodius

Window {
    id: root
    width: 1040; height: 820
    visible: true
    color: "#04060c"
    title: "DECODIUS" + (assistant.callSign.length ? " — " + assistant.callSign : "")

    Shortcut { sequences: ["Escape"]; onActivated: assistant.interrupt() }

    // Temi colore: tinta base a riposo (blu / ambra / verde / rosso notte)
    property var themeColors: ["#36b6e0", "#ffb02e", "#3dffa0", "#ff6a6a"]
    property var themeNames: ["Blu", "Ambra", "Verde", "Notte"]
    property int themeIndex: 0
    property bool rosterOpen: false
    // Stato -> colore d'accento ("umore" di Decodius); a riposo usa il tema scelto
    property color accent: {
        switch (assistant.state) {
        case Assistant.Listening: return "#2de2ff";
        case Assistant.Thinking:  return "#ffb02e";
        case Assistant.Speaking:  return "#3dffa0";
        default:                  return root.themeColors[root.themeIndex];
        }
    }
    property string stateText: {
        switch (assistant.state) {
        case Assistant.Listening: return "IN ASCOLTO";
        case Assistant.Thinking:  return "ELABORO";
        case Assistant.Speaking:  return "RISPONDO";
        default:                  return "PRONTO";
        }
    }
    Behavior on accent { ColorAnimation { duration: 450; easing.type: Easing.InOutQuad } }

    AudioAnalyzer { id: analyzer }
    Assistant     { id: assistant }
    Component.onCompleted: {
        analyzer.start()
        if (assistant.needsCallSign) callDialog.open()   // primo avvio: chiedi il nominativo
        else showWelcome()
    }
    function showWelcome() {
        var c = assistant.callSign.length ? assistant.callSign : "OM"
        chatModel.append({ role: "assistant",
            body: "Ciao **" + c + "**, sono **Decodius**, il tuo assistente radioamatoriale. Chiedimi qualcosa su bande, propagazione, antenne, codice Morse, satelliti, FT8/FT2, normativa…" })
    }

    // Pulsante "pill" riutilizzabile (componente inline)
    component PillButton: Button {
        property color baseColor: "#10202b"
        property color fg: root.accent
        Layout.preferredHeight: 40
        leftPadding: 16; rightPadding: 16
        background: Rectangle {
            radius: 12; color: parent.down ? Qt.darker(baseColor, 1.2) : baseColor
            border.color: root.accent; border.width: 1
            scale: parent.down ? 0.95 : 1.0
            Behavior on scale { NumberAnimation { duration: 90 } }
        }
        contentItem: Text { text: parent.text; color: fg; font.bold: true
            font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter }
    }

    // Chat: storico conversazione
    ListModel { id: chatModel }
    property int asstIndex: -1
    function send() {
        var t = input.text.trim()
        if (t.length === 0 && !assistant.hasImage) return
        chatModel.append({ role: "user", body: t.length ? t : "🖼️ (immagine allegata)" })
        chatModel.append({ role: "assistant", body: "" })
        asstIndex = chatModel.count - 1
        assistant.sendText(input.text)
        input.text = ""
    }
    Connections {
        target: assistant
        function onLastResponseChanged() {
            if (root.asstIndex >= 0 && root.asstIndex < chatModel.count)
                chatModel.setProperty(root.asstIndex, "body", assistant.lastResponse)
        }
        function onConfirmationRequested(title, detail) {
            confirmDialog.title = title; confirmDetail.text = detail; confirmDialog.open()
        }
    }

    // ───────────────── SFONDO: aurora animata (blob sfocati) ─────────────────
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#0a1320" }
            GradientStop { position: 1.0; color: "#04060c" }
        }
    }
    Item {
        id: aurora
        anchors.fill: parent
        visible: false
        layer.enabled: true
        Repeater {
            model: 4
            delegate: Rectangle {
                property real baseX: (index * 0.27 + 0.1) * root.width
                property real baseY: (index % 2 === 0 ? 0.25 : 0.7) * root.height
                width: 360 + index * 90; height: width; radius: width / 2
                x: baseX - width/2; y: baseY - height/2
                color: index % 2 === 0 ? root.accent : Qt.lighter(root.accent, 1.4)
                opacity: 0.22
                // movimento morbido e lento
                XAnimator on x { from: baseX - width/2 - 80; to: baseX - width/2 + 80
                    duration: 9000 + index*2500; loops: Animation.Infinite; easing.type: Easing.InOutSine }
                YAnimator on y { from: baseY - height/2 - 60; to: baseY - height/2 + 60
                    duration: 11000 + index*2200; loops: Animation.Infinite; easing.type: Easing.InOutSine }
                scale: 1.0 + analyzer.rms * 0.35
                Behavior on scale { NumberAnimation { duration: 160 } }
            }
        }
    }
    MultiEffect {
        anchors.fill: aurora
        source: aurora
        blurEnabled: true
        blur: 1.0
        blurMax: 64
        opacity: 0.9
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 16

        // ───────────────── HEADER: ORB centrale pulsante ─────────────────
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 360

            // wordmark in alto a sinistra
            Column {
                anchors.left: parent.left; anchors.top: parent.top; spacing: 2
                Text { text: "DECODIUS"; color: "#eaf6fb"; font.bold: true
                       font.pixelSize: 20; font.letterSpacing: 4 }
                Text { text: "assistente radioamatoriale" + (assistant.callSign.length ? " · " + assistant.callSign : "")
                       color: "#6f93a4"; font.pixelSize: 11; font.letterSpacing: 1 }
                Text { text: "v" + Qt.application.version
                       color: "#4d6b78"; font.pixelSize: 10; font.letterSpacing: 1 }
            }

            // HUD stazione live (stato di Decodium 4) — in alto a destra
            Rectangle {
                anchors.right: parent.right; anchors.top: parent.top
                width: Math.max(hudCol.implicitWidth + 28, 200); height: hudCol.implicitHeight + 18
                radius: 10
                color: Qt.rgba(0.04, 0.10, 0.14, 0.55)
                border.color: assistant.stationOnline ? Qt.rgba(0.3,0.95,0.6,0.5) : Qt.rgba(0.5,0.5,0.5,0.3)
                border.width: 1
                Column {
                    id: hudCol
                    anchors.centerIn: parent; spacing: 3
                    Row {
                        spacing: 6
                        Rectangle { width: 8; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter
                                    color: assistant.stationOnline ? "#3df58a" : "#ff5d72" }
                        Text { text: "DECODIUM " + (assistant.stationOnline ? "ONLINE" : "offline")
                               color: assistant.stationOnline ? "#9fe7c0" : "#8aa0ab"
                               font.pixelSize: 10; font.bold: true; font.letterSpacing: 1 }
                    }
                    Text { visible: assistant.stationOnline; text: assistant.stationLine1
                           color: "#eaf6fb"; font.pixelSize: 13; font.family: "Consolas"; font.bold: true }
                    Text { visible: assistant.stationOnline; text: assistant.stationLine2
                           color: "#7fb3c8"; font.pixelSize: 11; font.family: "Consolas" }
                }
            }

            // ORB (più grande)
            Item {
                id: orb
                width: 330; height: 330
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top; anchors.topMargin: 4

                // respiro continuo (pulse) + reazione audio
                property real pulse: 0
                property real react: analyzer.rms
                property bool talking: assistant.state === Assistant.Speaking
                                       || assistant.state === Assistant.Listening
                SequentialAnimation on pulse {
                    loops: Animation.Infinite
                    NumberAnimation { to: 1; duration: 1700; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 0; duration: 1700; easing.type: Easing.InOutSine }
                }

                // Anello spettrale: barre radiali alimentate dallo SPETTRO AUDIO REALE (FFT).
                Item {
                    anchors.fill: parent
                    z: 3
                    Repeater {
                        model: analyzer.levels
                        Item {
                            width: orb.width; height: orb.height
                            rotation: index * 360.0 / Math.max(1, analyzer.levels.length)
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: 14
                                width: 3
                                height: 4 + Math.min(1, modelData) * 58
                                radius: 1.5
                                antialiasing: true
                                color: Qt.rgba(0.25, 0.95, 0.6, 0.30 + Math.min(1, modelData) * 0.65)
                            }
                        }
                    }
                }
                Behavior on react { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }

                // ONDE CONCENTRICHE (ripple): si propagano mentre Decodius parla
                Repeater {
                    model: 4
                    delegate: Rectangle {
                        id: rip
                        anchors.centerIn: parent
                        width: 170; height: 170; radius: 85
                        color: "transparent"
                        border.color: Qt.lighter(root.accent, 1.2); border.width: 2
                        opacity: 0; scale: 0.7
                        visible: orb.talking
                        SequentialAnimation {
                            running: orb.talking
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 520 }
                            ParallelAnimation {
                                NumberAnimation { target: rip; property: "scale"; from: 0.7; to: 2.5; duration: 2000; easing.type: Easing.OutQuad }
                                NumberAnimation { target: rip; property: "opacity"; from: 0.55; to: 0.0; duration: 2000; easing.type: Easing.OutQuad }
                            }
                        }
                    }
                }

                // aloni concentrici soffici
                Repeater {
                    model: 4
                    delegate: Rectangle {
                        anchors.centerIn: parent
                        width: 190 + index*36; height: width; radius: width/2
                        color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.07 - index*0.014)
                        scale: 1.0 + orb.pulse*(0.05+index*0.03) + orb.react*(0.6+index*0.35)
                    }
                }

                // anelli energetici rotanti
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        anchors.centerIn: parent
                        width: 220 + index*46; height: width; radius: width/2
                        color: "transparent"
                        border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.28 - index*0.10)
                        border.width: index === 0 ? 1.5 : 1
                        RotationAnimation on rotation { from: 0; to: index % 2 ? 360 : -360
                            duration: 16000 + index*9000; loops: Animation.Infinite }
                        Repeater {
                            model: 84
                            delegate: Item { anchors.fill: parent; rotation: index*(360/84)
                                Rectangle { width: 1.6; height: (index%6===0)?9:4; radius:1
                                    color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.20)
                                    x: parent.width/2 - width/2; y: 1 } }
                        }
                    }
                }

                // PARTICELLE in orbita con SCIA luminosa
                Repeater {
                    model: 7
                    delegate: Item {
                        id: orbit
                        anchors.centerIn: parent; width: orb.width; height: orb.height
                        property int pidx: index
                        property real rad: orb.width/2 - (14 + (pidx % 3)*26)
                        RotationAnimation on rotation { from: pidx*51
                            to: pidx*51 + ((pidx % 2) ? 360 : -360)
                            duration: 4200 + pidx*1300; loops: Animation.Infinite }
                        Repeater {
                            model: 9   // testa + scia lunga
                            delegate: Item {
                                anchors.fill: parent
                                rotation: index * ((orbit.pidx % 2) ? 3.5 : -3.5)
                                Rectangle {
                                    width: 9 - index*0.7; height: width; radius: width/2
                                    color: Qt.lighter(root.accent, 1.8)
                                    x: parent.width/2 - width/2
                                    y: parent.height/2 - orbit.rad
                                    opacity: 0.95 - index*0.10
                                }
                            }
                        }
                    }
                }

                // anello reattivo sottile (feedback voce)
                Rectangle {
                    anchors.centerIn: parent; width: 176; height: 176; radius: 88
                    color: "transparent"
                    border.color: Qt.lighter(root.accent, 1.3); border.width: 2
                    opacity: 0.4 + orb.react*0.6
                    scale: 1.0 + orb.react*0.5
                }

                // SFERA centrale (look 3D) con PLASMA rotante e ONDA SINUSOIDALE interna
                Rectangle {
                    id: sphere
                    anchors.centerIn: parent; width: 158; height: 158; radius: 79
                    scale: 1.0 + orb.pulse*0.05 + orb.react*0.55
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.lighter(root.accent, 1.9) }
                        GradientStop { position: 0.55; color: root.accent }
                        GradientStop { position: 1.0; color: Qt.darker(root.accent, 1.5) }
                    }
                    // PLASMA: due dischi-gradiente che ruotano in versi opposti.
                    // Sono cerchi (radius=raggio) -> ruotando restano dentro la sfera.
                    Rectangle {
                        anchors.fill: parent; radius: width/2; opacity: 0.55
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: Qt.rgba(1,1,1,0.38) }
                            GradientStop { position: 0.5; color: Qt.rgba(1,1,1,0.0) }
                            GradientStop { position: 1.0; color: Qt.rgba(0,0.10,0.16,0.45) }
                        }
                        RotationAnimation on rotation { from: 0; to: 360; duration: 9000; loops: Animation.Infinite }
                    }
                    Rectangle {
                        anchors.fill: parent; radius: width/2; opacity: 0.40
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.0) }
                            GradientStop { position: 0.5; color: Qt.lighter(root.accent, 1.6) }
                            GradientStop { position: 1.0; color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.0) }
                        }
                        RotationAnimation on rotation { from: 360; to: 0; duration: 13000; loops: Animation.Infinite }
                    }
                    // highlight lucido in alto a sinistra
                    Rectangle {
                        x: 34; y: 24; width: 58; height: 38; radius: 26
                        color: Qt.rgba(1,1,1,0.40); rotation: -20
                    }
                    // ONDA SINUSOIDALE continua (Canvas), ampiezza reattiva all'audio
                    Canvas {
                        id: wave
                        anchors.centerIn: parent
                        width: 122; height: 64
                        property real phase: 0
                        property real amp: 4 + analyzer.rms * 40
                        Behavior on amp { NumberAnimation { duration: 110 } }
                        NumberAnimation on phase {
                            from: 0; to: 2 * Math.PI; duration: 1500; loops: Animation.Infinite
                        }
                        onPhaseChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.reset();
                            ctx.lineWidth = 2.6;
                            ctx.strokeStyle = "rgba(3,18,24,0.9)";
                            ctx.lineJoin = "round";
                            ctx.beginPath();
                            for (var x = 0; x <= width; x++) {
                                var t = x / width;
                                var env = Math.sin(t * Math.PI);          // smorza ai bordi
                                var y = height/2 + Math.sin(t*Math.PI*4 + phase) * amp * env;
                                if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
                            }
                            ctx.stroke();
                        }
                    }
                }

                layer.enabled: true
                MouseArea { anchors.fill: parent; onClicked: assistant.interrupt() }
            }
            // BLOOM dell'orb
            MultiEffect {
                anchors.fill: orb; source: orb
                blurEnabled: true; blur: 0.8; blurMax: 40
                brightness: 0.12; saturation: 0.25; opacity: 0.6
                z: -1
            }

            // pill di stato sotto l'orb
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                radius: 14; height: 30; width: pill.implicitWidth + 34
                color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
                border.color: root.accent; border.width: 1
                Row {
                    id: pill; anchors.centerIn: parent; spacing: 8
                    Rectangle { width: 9; height: 9; radius: 4.5; color: root.accent
                        anchors.verticalCenter: parent.verticalCenter
                        SequentialAnimation on opacity { loops: Animation.Infinite
                            NumberAnimation { to: 0.3; duration: 700 }
                            NumberAnimation { to: 1.0; duration: 700 } }
                    }
                    Text { text: root.stateText; color: root.accent; font.bold: true
                           font.pixelSize: 12; font.letterSpacing: 2
                           anchors.verticalCenter: parent.verticalCenter }
                }
            }
        }

        // ───────────────── CHAT (bolle + streaming) ─────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 18
            color: Qt.rgba(1, 1, 1, 0.03)
            border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.18)
            border.width: 1

            ListView {
                id: chat
                anchors.fill: parent
                anchors.margins: 14
                model: chatModel
                spacing: 14
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                onCountChanged: positionViewAtEnd()
                onContentHeightChanged: positionViewAtEnd()

                // animazione d'ingresso delle bolle: salgono + sfumano + scalano
                add: Transition {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 320; easing.type: Easing.OutCubic }
                    NumberAnimation { property: "y"; from: 26; duration: 360; easing.type: Easing.OutCubic }
                    NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: 320; easing.type: Easing.OutBack }
                }
                displaced: Transition { NumberAnimation { properties: "y"; duration: 220; easing.type: Easing.OutCubic } }

                delegate: Item {
                    width: chat.width
                    height: bubble.height + 6
                    property bool isUser: role === "user"
                    property bool thinking: !isUser && body.length === 0

                    Rectangle {
                        id: bubble
                        anchors.right: isUser ? parent.right : undefined
                        anchors.left: isUser ? undefined : parent.left
                        width: Math.min(chat.width * 0.80, contentCol.implicitWidth + 30)
                        height: contentCol.implicitHeight + 22
                        radius: 16
                        color: isUser ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
                                      : Qt.rgba(1, 1, 1, 0.045)
                        border.width: 1
                        border.color: isUser ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.55)
                                             : Qt.rgba(1, 1, 1, 0.10)

                        Column {
                            id: contentCol
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 11
                            spacing: 4

                            Text {
                                text: isUser ? "TU" : "DECODIUS"
                                color: isUser ? root.accent : Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.85)
                                font.bold: true; font.pixelSize: 10; font.letterSpacing: 1.5
                            }

                            // indicatore "sta scrivendo" (tre puntini) finché il corpo è vuoto
                            Row {
                                visible: thinking
                                spacing: 6
                                Repeater {
                                    model: 3
                                    delegate: Rectangle {
                                        width: 9; height: 9; radius: 4.5; color: root.accent
                                        SequentialAnimation on opacity {
                                            loops: Animation.Infinite
                                            PauseAnimation { duration: index * 180 }
                                            NumberAnimation { to: 1.0; duration: 280 }
                                            NumberAnimation { to: 0.25; duration: 280 }
                                            PauseAnimation { duration: (2 - index) * 180 }
                                        }
                                    }
                                }
                            }

                            Text {
                                visible: !thinking
                                width: Math.min(chat.width * 0.80 - 22, implicitWidth)
                                text: body
                                color: "#e7f3f8"
                                wrapMode: Text.WordWrap
                                textFormat: Text.MarkdownText
                                font.pixelSize: 15; lineHeight: 1.15
                                onLinkActivated: (l) => Qt.openUrlExternally(l)
                            }
                        }
                    }
                }
            }
        }

        // ───────────────── QUICK ACTIONS (solo letture) ─────────────────
        Flow {
            Layout.fillWidth: true
            spacing: 8
            Repeater {
                model: [
                    { t: "📡 In banda", q: "cosa c'è in banda adesso?" },
                    { t: "🌞 Propagazione", q: "com'è la propagazione?" },
                    { t: "🌍 DX", q: "quali DX ci sono ora nel cluster?" },
                    { t: "🧭 Stato", q: "qual è lo stato di Decodium?" }
                ]
                Rectangle {
                    radius: 14; height: 30
                    width: qaLabel.implicitWidth + 24
                    color: qaMouse.containsMouse ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.18)
                                                 : Qt.rgba(1,1,1,0.05)
                    border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.35); border.width: 1
                    Behavior on color { ColorAnimation { duration: 150 } }
                    Text { id: qaLabel; anchors.centerIn: parent; text: modelData.t
                           color: "#cfe6f0"; font.pixelSize: 12 }
                    MouseArea { id: qaMouse; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: assistant.sendText(modelData.q) }
                }
            }
        }

        // ───────────────── INPUT BAR ─────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            radius: 16
            color: Qt.rgba(1, 1, 1, 0.04)
            border.width: 1
            border.color: input.activeFocus
                ? root.accent
                : Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.25)
            Behavior on border.color { ColorAnimation { duration: 200 } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16; anchors.rightMargin: 10
                anchors.topMargin: 8; anchors.bottomMargin: 8
                spacing: 10

                TextField {
                    id: input
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    placeholderText: "Chiedi a Decodius…"
                    placeholderTextColor: "#5e7c8a"
                    color: "#eaf6fb"; font.pixelSize: 15
                    background: Item {}
                    verticalAlignment: TextInput.AlignVCenter
                    onAccepted: send()
                }

                Button {
                    text: "Invia ➤"
                    onClicked: send()
                    Layout.preferredHeight: 40; leftPadding: 18; rightPadding: 18
                    background: Rectangle { radius: 12; scale: parent.down ? 0.95 : 1.0
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: Qt.lighter(root.accent, 1.3) }
                            GradientStop { position: 1.0; color: root.accent } }
                        Behavior on scale { NumberAnimation { duration: 90 } } }
                    contentItem: Text { text: parent.text; color: "#04121a"; font.bold: true
                        font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter }
                }
                PillButton {
                    text: "⏹"; fg: "#ff5d72"
                    visible: assistant.state === Assistant.Thinking || assistant.state === Assistant.Speaking
                    onClicked: assistant.interrupt()
                }
                PillButton {
                    text: assistant.hasImage ? "📷✓" : "📷"
                    fg: assistant.hasImage ? "#04121a" : root.accent
                    baseColor: assistant.hasImage ? root.accent : "#10202b"
                    onClicked: if (assistant.hasImage) assistant.clearImage()
                }
                PillButton {
                    text: assistant.alwaysListening ? "🎤" : "🎤"
                    fg: assistant.alwaysListening ? "#04121a" : root.accent
                    baseColor: assistant.alwaysListening ? root.accent : "#10202b"
                    onClicked: assistant.setListening(!assistant.alwaysListening)
                }
                // Mani libere (wake-word "Decodius")
                PillButton {
                    text: "🎙"
                    fg: assistant.wakeWord ? "#04121a" : root.accent
                    baseColor: assistant.wakeWord ? root.accent : "#10202b"
                    onClicked: assistant.setWakeWord(!assistant.wakeWord)
                }
                // Pilota automatico (operatore autonomo)
                PillButton {
                    text: "🤖"
                    fg: assistant.autoPilot ? "#04121a" : "#ff8c42"
                    baseColor: assistant.autoPilot ? "#ff8c42" : "#10202b"
                    onClicked: assistant.setAutoPilot(!assistant.autoPilot)
                }
                // Selettore voce (cicla: Giuseppe/Diego/Isabella/Elsa)
                PillButton {
                    text: "🗣" + (assistant.voice.length ? assistant.voice.charAt(0).toUpperCase() : "")
                    fg: root.accent
                    baseColor: "#10202b"
                    onClicked: assistant.cycleVoice()
                }
                // Tema colore (cicla blu/ambra/verde/notte)
                PillButton {
                    text: "🎨"
                    fg: root.accent
                    baseColor: "#10202b"
                    onClicked: root.themeIndex = (root.themeIndex + 1) % root.themeColors.length
                }
                // Call Roster (pannello laterale stazioni in banda)
                PillButton {
                    text: "📋"
                    fg: root.rosterOpen ? "#04121a" : root.accent
                    baseColor: root.rosterOpen ? root.accent : "#10202b"
                    onClicked: root.rosterOpen = !root.rosterOpen
                }
            }
        }
    }

    // ───────── CALL ROSTER laterale (stazioni in banda, live da Decodium) ─────────
    Rectangle {
        id: rosterPanel
        width: 320; height: 470
        property bool mapView: false              // false=lista, true=mappa
        x: parent.width - width - 20              // posizione iniziale (il drag la sovrascrive)
        y: 70
        visible: root.rosterOpen
        opacity: root.rosterOpen ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        color: Qt.rgba(0.03, 0.08, 0.11, 0.95)
        radius: 10
        border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.45); border.width: 1
        z: 50

        // ── Barra titolo TRASCINABILE ──
        Rectangle {
            id: rosterTitleBar
            width: parent.width; height: 28
            anchors.top: parent.top
            radius: 10
            color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                drag.target: rosterPanel
                drag.axis: Drag.XAndYAxis
                drag.minimumX: 0; drag.maximumX: root.width - rosterPanel.width
                drag.minimumY: 0; drag.maximumY: root.height - rosterPanel.height
            }
            Text { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter
                   text: "⠿  CALL ROSTER"; color: root.accent; font.bold: true
                   font.pixelSize: 12; font.letterSpacing: 1 }
            Rectangle {
                anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter
                width: 20; height: 20; radius: 10; color: closeMouse.containsMouse ? "#ff5d72" : "transparent"
                Text { anchors.centerIn: parent; text: "✕"; color: "#cfe9f2"; font.pixelSize: 12 }
                MouseArea { id: closeMouse; anchors.fill: parent; hoverEnabled: true
                            onClicked: root.rosterOpen = false }
            }
        }

        Column {
            anchors.fill: parent; anchors.topMargin: 36; anchors.margins: 12; spacing: 8
            Row {
                width: parent.width; spacing: 6
                Item { width: parent.width - 110; height: 1 }
                // toggle Lista / Mappa
                Rectangle { width: 44; height: 22; radius: 11
                    color: !rosterPanel.mapView ? root.accent : Qt.rgba(1,1,1,0.06)
                    Text { anchors.centerIn: parent; text: "Lista"; font.pixelSize: 10
                           color: !rosterPanel.mapView ? "#04121a" : "#9fc0cf" }
                    MouseArea { anchors.fill: parent; onClicked: rosterPanel.mapView = false } }
                Rectangle { width: 48; height: 22; radius: 11
                    color: rosterPanel.mapView ? root.accent : Qt.rgba(1,1,1,0.06)
                    Text { anchors.centerIn: parent; text: "🗺 Mappa"; font.pixelSize: 10
                           color: rosterPanel.mapView ? "#04121a" : "#9fc0cf" }
                    MouseArea { anchors.fill: parent; onClicked: rosterPanel.mapView = true } }
                Text { text: assistant.callRoster.length + ""; color: "#7fb3c8"; font.pixelSize: 12
                       anchors.verticalCenter: parent.verticalCenter }
            }
            Rectangle { width: parent.width; height: 1
                        color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.3) }

            // ── VISTA MAPPA: planisfero reale + marker delle stazioni ──
            Rectangle {
                visible: rosterPanel.mapView
                width: parent.width; height: width / 2          // equirettangolare 2:1
                color: "#06121b"; radius: 6; clip: true
                Image {
                    id: worldMap
                    anchors.fill: parent
                    source: "world.png"
                    fillMode: Image.Stretch
                    opacity: 0.92
                }
                Repeater {
                    model: assistant.callRoster
                    Rectangle {
                        visible: modelData.lat !== undefined
                        width: modelData.isCq ? 9 : 7; height: width; radius: width/2
                        x: worldMap.width  * (modelData.lon + 180) / 360 - width/2
                        y: worldMap.height * (90 - modelData.lat) / 180 - height/2
                        color: modelData.isCq ? "#3df58a" : "#ffd24a"
                        border.color: "#04121a"; border.width: 1
                        ToolTip.visible: mkMouse.containsMouse
                        ToolTip.text: modelData.call + " · " + (modelData.grid || "") + " · " + modelData.db + " dB"
                        MouseArea { id: mkMouse; anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: assistant.sendText("dimmi qualcosa su " + modelData.call) }
                    }
                }
                Text { anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: 4
                       text: "🟢 CQ  🟡 attive"; color: "#9fc0cf"; font.pixelSize: 9 }
            }
            Text { visible: assistant.callRoster.length === 0
                   text: assistant.stationOnline ? "Nessuna stazione in banda." : "Decodium offline."
                   color: "#6f93a4"; font.pixelSize: 12; width: parent.width; wrapMode: Text.WordWrap }
            ListView {
                visible: !rosterPanel.mapView
                width: parent.width
                height: parent.height - 50
                clip: true; spacing: 4
                model: assistant.callRoster
                delegate: Rectangle {
                    width: ListView.view.width; height: 40; radius: 6
                    color: modelData.isCq ? Qt.rgba(0.25,0.95,0.6,0.12) : Qt.rgba(1,1,1,0.04)
                    border.color: modelData.isCq ? Qt.rgba(0.25,0.95,0.6,0.4) : "transparent"
                    border.width: 1
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: assistant.sendText("dimmi qualcosa su " + modelData.call) }
                    Row {
                        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 8
                        Column {
                            width: parent.width - 52; anchors.verticalCenter: parent.verticalCenter; spacing: 1
                            Row { spacing: 5
                                Text { text: modelData.call; color: "#eaf6fb"; font.bold: true
                                       font.pixelSize: 13; font.family: "Consolas" }
                                Text { visible: modelData.isCq; text: "CQ"; color: "#3df58a"
                                       font.pixelSize: 9; font.bold: true
                                       anchors.verticalCenter: parent.verticalCenter }
                            }
                            Text { text: modelData.country + (modelData.freq ? "  " + Math.round(modelData.freq) + " Hz" : "")
                                   color: "#6f93a4"; font.pixelSize: 10; elide: Text.ElideRight
                                   width: parent.width }
                        }
                        Text { text: (modelData.db > 0 ? "+" : "") + modelData.db
                               color: modelData.db >= -10 ? "#3df58a" : (modelData.db >= -18 ? "#ffb02e" : "#ff7a7a")
                               font.pixelSize: 12; font.family: "Consolas"; font.bold: true
                               anchors.verticalCenter: parent.verticalCenter }
                    }
                }
            }
        }
    }

    // ───────── Drop immagine (vision) ─────────
    DropArea {
        anchors.fill: parent
        onEntered: (drag) => { if (drag.hasUrls) dropHint.visible = true }
        onExited: dropHint.visible = false
        onDropped: (drop) => {
            dropHint.visible = false
            if (drop.hasUrls && drop.urls.length > 0) { assistant.attachImage(drop.urls[0]); drop.accept() }
        }
    }
    Rectangle {
        id: dropHint; anchors.fill: parent; visible: false
        color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.12)
        border.color: root.accent; border.width: 2; radius: 16
        Text { anchors.centerIn: parent; text: "Rilascia l'immagine per allegarla a Decodius"
               color: root.accent; font.bold: true; font.pixelSize: 20 }
    }

    // ───────── Dialog conferma strumenti in scrittura ─────────
    Dialog {
        id: confirmDialog; modal: true; anchors.centerIn: parent
        width: Math.min(root.width - 80, 640); padding: 18; closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: 14; color: "#0c141d"
            border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.5); border.width: 1 }
        header: Text { text: confirmDialog.title; color: root.accent; font.bold: true; font.pixelSize: 16; padding: 14 }
        contentItem: Flickable {
            implicitHeight: Math.min(contentHeight, 320); contentHeight: confirmDetail.height; clip: true
            Text { id: confirmDetail; width: confirmDialog.availableWidth; color: "#cfe9f2"
                   wrapMode: Text.WrapAtWordBoundaryOrAnywhere; font.pixelSize: 13; font.family: "Consolas" }
        }
        footer: RowLayout {
            spacing: 10; Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: "Annulla"; onClicked: { assistant.resolveConfirmation(false); confirmDialog.close() }
                background: Rectangle { radius: 8; color: "#0c141d"; border.color: "#7a8a95"; border.width: 1 }
                contentItem: Text { text: parent.text; color: "#cfe9f2"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
            Button { text: "Conferma"; onClicked: { assistant.resolveConfirmation(true); confirmDialog.close() }
                background: Rectangle { radius: 8; color: root.accent; opacity: 0.9 }
                contentItem: Text { text: parent.text; color: "#04121a"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                Layout.rightMargin: 14; Layout.bottomMargin: 12 }
            Layout.bottomMargin: 6
        }
    }

    // ───────── Primo avvio: personalizzazione del nominativo (Call/QRZ) ─────────
    Dialog {
        id: callDialog; modal: true; anchors.centerIn: parent
        width: Math.min(root.width - 120, 460); padding: 22
        closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: 16; color: "#0c141d"
            border.color: root.accent; border.width: 1 }
        contentItem: ColumnLayout {
            spacing: 14
            Text { text: "Benvenuto in Decodius"; color: "#eaf6fb"; font.bold: true; font.pixelSize: 20 }
            Text { text: "Inserisci il tuo nominativo (Call/QRZ) per personalizzare l'assistente:"
                   color: "#9fc0cf"; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            TextField {
                id: callInput; Layout.fillWidth: true
                placeholderText: "es. IK1ABC"; placeholderTextColor: "#5e7c8a"
                color: "#eaf6fb"; font.pixelSize: 18; font.capitalization: Font.AllUppercase
                font.letterSpacing: 2; horizontalAlignment: TextInput.AlignHCenter
                background: Rectangle { radius: 10; color: "#0a131c"
                    border.color: callInput.activeFocus ? root.accent
                        : Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.35); border.width: 1 }
                onAccepted: callDialog.confirmCall()
                Component.onCompleted: forceActiveFocus()
            }
            Button {
                text: "Inizia ▸"; Layout.fillWidth: true; Layout.preferredHeight: 44
                enabled: callInput.text.trim().length >= 3
                onClicked: callDialog.confirmCall()
                background: Rectangle { radius: 12; opacity: parent.enabled ? 1 : 0.4
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.lighter(root.accent, 1.3) }
                        GradientStop { position: 1.0; color: root.accent } } }
                contentItem: Text { text: parent.text; color: "#04121a"; font.bold: true; font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            }
        }
        function confirmCall() {
            var c = callInput.text.trim()
            if (c.length < 3) return
            assistant.setCallSign(c)
            callDialog.close()
            showWelcome()
        }
    }

    // ───────── Wizard: scelta e attivazione del "cervello" (LLM) ─────────
    Connections {
        target: assistant
        function onBrainChanged() {
            if (assistant.needsBrainSetup && !assistant.needsCallSign
                    && !callDialog.visible && !brainDialog.visible)
                brainDialog.open()
        }
    }
    Dialog {
        id: brainDialog; modal: true; anchors.centerIn: parent
        width: Math.min(root.width - 80, 560); padding: 22
        closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: 16; color: "#0c141d"; border.color: root.accent; border.width: 1 }
        contentItem: ColumnLayout {
            spacing: 12
            Text { text: "🧠 Scegli il cervello di Decodius"; color: "#eaf6fb"; font.bold: true; font.pixelSize: 19 }
            Text { text: "Decodius ha bisogno di un modello AI per ragionare. Stato attuale: " + assistant.brainStatus
                   color: "#9fc0cf"; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }

            // Opzione 1 — Ollama (setup automatico)
            Rectangle { Layout.fillWidth: true; radius: 10; color: Qt.rgba(1,1,1,0.04)
                border.color: Qt.rgba(root.accent.r,root.accent.g,root.accent.b,0.35); border.width: 1
                implicitHeight: o1.implicitHeight + 20
                ColumnLayout { id: o1; anchors.fill: parent; anchors.margins: 12; spacing: 6
                    Text { text: "1 ·  Ollama + qwen3:1.7b  (locale, consigliato)"; color: root.accent; font.bold: true; font.pixelSize: 13 }
                    Text { text: "Installa Ollama, ti fa accedere con un clic e prepara il modello. Serve internet e un account Ollama gratuito."
                           color: "#9fc0cf"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Button { text: "⚙  Avvia setup automatico"; onClicked: assistant.runBrainSetup()
                        background: Rectangle { radius: 9; color: root.accent }
                        contentItem: Text { text: parent.text; color: "#04121a"; font.bold: true; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; padding: 6 } }
                }
            }

            // Opzione 2 — Provider cloud (form)
            Rectangle { Layout.fillWidth: true; radius: 10; color: Qt.rgba(1,1,1,0.04)
                border.color: Qt.rgba(root.accent.r,root.accent.g,root.accent.b,0.35); border.width: 1
                implicitHeight: o2.implicitHeight + 20
                ColumnLayout { id: o2; anchors.fill: parent; anchors.margins: 12; spacing: 6
                    Text { text: "2 ·  Provider cloud  (NVIDIA / OpenRouter / DeepSeek / Gemini)"; color: root.accent; font.bold: true; font.pixelSize: 13 }
                    Text { text: "Crea una chiave gratuita (es. build.nvidia.com), incollala qui e salva."
                           color: "#9fc0cf"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    TextField { id: provUrl; Layout.fillWidth: true; color: "#eaf6fb"; font.pixelSize: 12
                        text: "https://integrate.api.nvidia.com/v1"; placeholderText: "base_url"
                        background: Rectangle { radius: 8; color: "#0a131c"; border.color: Qt.rgba(1,1,1,0.15); border.width: 1 } }
                    TextField { id: provKey; Layout.fillWidth: true; color: "#eaf6fb"; font.pixelSize: 12
                        placeholderText: "api_key (es. nvapi-...)"; echoMode: TextInput.PasswordEchoOnEdit
                        background: Rectangle { radius: 8; color: "#0a131c"; border.color: Qt.rgba(1,1,1,0.15); border.width: 1 } }
                    TextField { id: provModel; Layout.fillWidth: true; color: "#eaf6fb"; font.pixelSize: 12
                        text: "meta/llama-3.1-8b-instruct"; placeholderText: "model"
                        background: Rectangle { radius: 8; color: "#0a131c"; border.color: Qt.rgba(1,1,1,0.15); border.width: 1 } }
                    Button { text: "💾  Salva provider"; enabled: provKey.text.trim().length > 8
                        onClicked: { assistant.saveProvider(provUrl.text, provKey.text, provModel.text); brainDialog.close() }
                        background: Rectangle { radius: 9; opacity: parent.enabled?1:0.4; color: root.accent }
                        contentItem: Text { text: parent.text; color: "#04121a"; font.bold: true; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; padding: 6 } }
                }
            }

            // Opzione 3 — locale (istruzioni)
            Text { text: "3 ·  Modello locale (offline): installa Ollama, poi nel prompt:  ollama pull qwen2.5:7b  e scrivi 'qwen2.5:7b' in decodius_model.txt."
                   color: "#7fb3c8"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }

            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Button { text: "↻ Ricontrolla"; onClicked: assistant.recheckBrain()
                    background: Rectangle { radius: 9; color: "#1a2a36"; border.color: root.accent; border.width: 1 }
                    contentItem: Text { text: parent.text; color: root.accent; font.pixelSize: 12; padding: 6
                        horizontalAlignment: Text.AlignHCenter } }
                Item { Layout.fillWidth: true }
                Button { text: "Chiudi"; onClicked: brainDialog.close()
                    background: Rectangle { radius: 9; color: "#1a2a36" }
                    contentItem: Text { text: parent.text; color: "#9fc0cf"; font.pixelSize: 12; padding: 6
                        horizontalAlignment: Text.AlignHCenter } }
            }
        }
    }

    // ───────── SCHEDA NOMINATIVO (QRZ-like) — overlay con mappa + dati HamQTH ─────────
    Rectangle {
        id: cardOverlay
        anchors.fill: parent
        visible: assistant.cardVisible
        opacity: assistant.cardVisible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        color: Qt.rgba(0, 0, 0, 0.55)
        z: 100
        MouseArea { anchors.fill: parent; onClicked: assistant.hideCard() }   // clic fuori = chiudi

        Rectangle {
            id: cardBox
            anchors.centerIn: parent
            width: Math.min(parent.width - 60, 540); height: 430
            radius: 16
            color: Qt.rgba(0.04, 0.09, 0.13, 0.97)
            border.color: root.accent; border.width: 1
            MouseArea { anchors.fill: parent }   // assorbe i clic dentro la card
            readonly property var c: assistant.callCard

            Column {
                anchors.fill: parent; anchors.margins: 16; spacing: 10

                Row {
                    width: parent.width
                    Text { text: cardBox.c.call || "—"; color: root.accent; font.bold: true
                           font.pixelSize: 26; font.family: "Consolas"; font.letterSpacing: 2 }
                    Item { width: parent.width - 200; height: 1 }
                    Rectangle { width: 26; height: 26; radius: 13; color: cl.containsMouse ? "#ff5d72" : "#1a2a36"
                        Text { anchors.centerIn: parent; text: "✕"; color: "#cfe9f2"; font.pixelSize: 13 }
                        MouseArea { id: cl; anchors.fill: parent; hoverEnabled: true; onClicked: assistant.hideCard() } }
                }

                Text { visible: cardBox.c.loading === true; text: "Caricamento da HamQTH…"
                       color: "#9fc0cf"; font.pixelSize: 13 }
                Text { visible: cardBox.c.error !== undefined; text: "⚠ " + (cardBox.c.error || "")
                       color: "#ffb02e"; font.pixelSize: 13; wrapMode: Text.WordWrap; width: parent.width }

                Rectangle {
                    width: parent.width; height: 180; radius: 8; clip: true; color: "#06121b"
                    visible: cardBox.c.loading !== true && cardBox.c.error === undefined
                    Image { id: cardMap; anchors.fill: parent; source: "world.png"; fillMode: Image.Stretch; opacity: 0.9 }
                    Rectangle {
                        visible: cardBox.c.lat !== undefined
                        width: 12; height: 12; radius: 6
                        x: cardMap.width  * ((cardBox.c.lon || 0) + 180) / 360 - 6
                        y: cardMap.height * (90 - (cardBox.c.lat || 0)) / 180 - 6
                        color: "#ff4a6a"; border.color: "#ffffff"; border.width: 1
                        SequentialAnimation on scale { loops: Animation.Infinite
                            NumberAnimation { to: 1.5; duration: 700 } NumberAnimation { to: 1.0; duration: 700 } }
                    }
                }

                Grid {
                    width: parent.width; columns: 2; rowSpacing: 5; columnSpacing: 12
                    visible: cardBox.c.loading !== true && cardBox.c.error === undefined
                    Text { text: "👤 " + (cardBox.c.name || "—"); color: "#eaf6fb"; font.pixelSize: 13; width: 250; elide: Text.ElideRight }
                    Text { text: "🌍 " + (cardBox.c.country || "—"); color: "#eaf6fb"; font.pixelSize: 13 }
                    Text { text: "🏙 " + (cardBox.c.qth || cardBox.c.city || "—"); color: "#cfe6f0"; font.pixelSize: 12; width: 250; elide: Text.ElideRight }
                    Text { text: "📍 " + (cardBox.c.grid || "—"); color: "#cfe6f0"; font.pixelSize: 12; font.family: "Consolas" }
                    Text { text: "✉ QSL: " + (cardBox.c.qsl || "—"); color: "#7fb3c8"; font.pixelSize: 11; width: 250; elide: Text.ElideRight }
                    Text { text: "ITU " + (cardBox.c.itu || "-") + " · CQ " + (cardBox.c.cq || "-"); color: "#7fb3c8"; font.pixelSize: 11 }
                }
                Text { text: "dati: HamQTH.com"; color: "#4d6b78"; font.pixelSize: 9
                       visible: cardBox.c.loading !== true && cardBox.c.error === undefined }
            }
        }
    }
}
