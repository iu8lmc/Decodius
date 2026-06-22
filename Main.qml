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

    // ───────── SCHEDA STAZIONE — HUD a TUTTO SCHERMO, stile Jarvis ─────────
    // Overlay translucido (si intravede l'app dietro) con pannelli che entrano in
    // cascata e si ritraggono all'uscita. Dati live da HamQTH (callCard).
    Item {
        id: cardOverlay
        anchors.fill: parent
        z: 100
        visible: assistant.cardVisible || reveal > 0.01
        readonly property var c: assistant.callCard
        readonly property bool ok: c.loading !== true && c.error === undefined
        readonly property color hud: root.accent
        property real reveal: 0   // 0 = nascosto, 1 = mostrato (pilota tutte le animazioni)
        // sotto-progresso 0..1 di reveal nell'intervallo [a,b]: entrate scaglionate
        function seg(a, b) { return Math.max(0, Math.min(1, (reveal - a) / (b - a))) }

        states: State { name: "on"; when: assistant.cardVisible
                        PropertyChanges { cardOverlay.reveal: 1 } }
        transitions: [
            Transition { to: "on";   NumberAnimation { property: "reveal"; duration: 540; easing.type: Easing.OutCubic } },
            Transition { from: "on"; NumberAnimation { property: "reveal"; duration: 320; easing.type: Easing.InCubic } }
        ]

        // sfondo translucido + chiusura cliccando nel vuoto
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0.01, 0.03, 0.06, 0.86 * cardOverlay.reveal)
            MouseArea { anchors.fill: parent; onClicked: assistant.hideCard() }
        }
        // scanline orizzontale che attraversa lo schermo all'apertura
        Rectangle {
            width: parent.width; height: 2; color: cardOverlay.hud
            opacity: 0.5 * cardOverlay.seg(0.0, 0.5) * (1 - cardOverlay.seg(0.5, 1.0))
            y: parent.height * cardOverlay.seg(0.0, 0.85)
        }

        // staffe angolari HUD (4 angoli) che "entrano"
        Repeater {
            model: [[1,1],[-1,1],[1,-1],[-1,-1]]
            delegate: Item {
                required property var modelData
                readonly property bool lft: modelData[0] > 0
                readonly property bool tp:  modelData[1] > 0
                width: 46; height: 46
                x: lft ? 26 : cardOverlay.width - 26 - width
                y: tp  ? 26 : cardOverlay.height - 26 - height
                opacity: cardOverlay.seg(0.0, 0.5)
                Rectangle { height: 3; color: cardOverlay.hud
                            width: 46 * cardOverlay.seg(0.05, 0.55)
                            x: parent.lft ? 0 : parent.width - width
                            y: parent.tp  ? 0 : parent.height - 3 }
                Rectangle { width: 3; color: cardOverlay.hud
                            height: 46 * cardOverlay.seg(0.05, 0.55)
                            x: parent.lft ? 0 : parent.width - 3
                            y: parent.tp  ? 0 : parent.height - height }
            }
        }

        // ── INTESTAZIONE: etichetta + CALLSIGN gigante (glow) + sottotitolo ──
        Column {
            id: headerCol
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * 0.07
            spacing: 4
            opacity: cardOverlay.seg(0.0, 0.45)
            transform: Translate { y: -22 * (1 - cardOverlay.seg(0.0, 0.45)) }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   text: "// SCHEDA  STAZIONE"; color: cardOverlay.hud; opacity: 0.7
                   font.pixelSize: 13; font.family: "Consolas"; font.letterSpacing: 6 }
            Text { id: bigCall
                   anchors.horizontalCenter: parent.horizontalCenter
                   text: cardOverlay.c.call || "—"; color: cardOverlay.hud
                   font.pixelSize: 78; font.bold: true; font.family: "Consolas"; font.letterSpacing: 8
                   layer.enabled: true
                   layer.effect: MultiEffect { shadowEnabled: true; shadowColor: cardOverlay.hud
                                               shadowBlur: 1.0; shadowVerticalOffset: 0; shadowHorizontalOffset: 0
                                               autoPaddingEnabled: true } }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   visible: cardOverlay.ok
                   text: (cardOverlay.c.name || "") + (cardOverlay.c.country ? "  ·  " + cardOverlay.c.country : "")
                   color: "#dff1f8"; font.pixelSize: 18; font.letterSpacing: 1 }
        }

        // ── STATO: scansione / errore ──
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10; opacity: cardOverlay.seg(0.2, 0.6)
            visible: !cardOverlay.ok
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   visible: cardOverlay.c.loading === true
                   text: "◌ SCANSIONE HAMQTH…"; color: cardOverlay.hud
                   font.pixelSize: 22; font.family: "Consolas"; font.letterSpacing: 3
                   SequentialAnimation on opacity { loops: Animation.Infinite; running: cardOverlay.c.loading === true
                       NumberAnimation { to: 0.35; duration: 600 } NumberAnimation { to: 1.0; duration: 600 } } }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   visible: cardOverlay.c.error !== undefined
                   text: "⚠  " + (cardOverlay.c.error || ""); color: "#ffb02e"
                   font.pixelSize: 18; width: 520; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
        }

        // ── MAPPA: pannello translucido grande con dot + ping + radar ──
        Rectangle {
            id: mapPanel
            visible: cardOverlay.ok
            anchors.left: parent.left; anchors.leftMargin: parent.width * 0.06
            anchors.verticalCenter: parent.verticalCenter; anchors.verticalCenterOffset: parent.height * 0.06
            width: parent.width * 0.52; height: parent.height * 0.48
            radius: 10; clip: true
            color: Qt.rgba(0.02, 0.07, 0.11, 0.55)
            border.color: cardOverlay.hud; border.width: 1
            opacity: cardOverlay.seg(0.12, 0.6)
            scale: 0.93 + 0.07 * cardOverlay.seg(0.12, 0.6)
            transform: Translate { x: -36 * (1 - cardOverlay.seg(0.12, 0.6)) }
            MouseArea { anchors.fill: parent }   // assorbe i clic (non chiude)

            Image { id: cardMap; anchors.fill: parent; anchors.margins: 1
                    source: "world.png"; fillMode: Image.Stretch; opacity: 0.55 }
            // reticolo
            Row { anchors.fill: parent
                Repeater { model: 12; delegate: Item { width: cardMap.width/12; height: cardMap.height
                    Rectangle { width: 1; height: parent.height; color: cardOverlay.hud; opacity: 0.10 } } } }
            Column { anchors.fill: parent
                Repeater { model: 6; delegate: Item { width: cardMap.width; height: cardMap.height/6
                    Rectangle { width: parent.width; height: 1; color: cardOverlay.hud; opacity: 0.10 } } } }

            // radar che ruota attorno alla stazione
            Item {
                id: radar
                visible: cardOverlay.c.lat !== undefined
                width: 2; height: 2
                x: cardMap.width  * ((cardOverlay.c.lon || 0) + 180) / 360
                y: cardMap.height * (90 - (cardOverlay.c.lat || 0)) / 180
                Rectangle { width: Math.max(cardMap.width, cardMap.height); height: 1.5
                            transformOrigin: Item.Left; color: cardOverlay.hud; opacity: 0.45 }
                RotationAnimator on rotation { from: 0; to: 360; duration: 4200
                                               loops: Animation.Infinite; running: cardOverlay.visible }
            }
            // ping anelli + punto stazione
            Item {
                visible: cardOverlay.c.lat !== undefined
                x: cardMap.width  * ((cardOverlay.c.lon || 0) + 180) / 360
                y: cardMap.height * (90 - (cardOverlay.c.lat || 0)) / 180
                Repeater { model: 2; delegate: Rectangle {
                    required property int index
                    anchors.centerIn: parent; radius: width/2; color: "transparent"
                    border.color: "#ff5d72"; border.width: 1.5
                    SequentialAnimation on width { loops: Animation.Infinite; running: cardOverlay.visible
                        PauseAnimation { duration: index * 700 }
                        NumberAnimation { from: 6; to: 56; duration: 1400; easing.type: Easing.OutQuad }
                        PropertyAction { value: 6 } }
                    height: width
                    opacity: 1 - (width - 6) / 50 } }
                Rectangle { anchors.centerIn: parent; width: 12; height: 12; radius: 6
                            color: "#ff4a6a"; border.color: "#ffffff"; border.width: 1.5 }
            }
            // coordinate in sovraimpressione
            Text { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 8
                   visible: cardOverlay.c.lat !== undefined
                   text: "LAT " + (cardOverlay.c.lat || 0).toFixed(2) + "  LON " + (cardOverlay.c.lon || 0).toFixed(2)
                   color: cardOverlay.hud; font.pixelSize: 11; font.family: "Consolas" }
            Text { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8
                   text: "● LIVE · HamQTH"; color: cardOverlay.hud; opacity: 0.7
                   font.pixelSize: 10; font.family: "Consolas" }
        }

        // ── LETTURA DATI: pannello con i campi HamQTH che entrano scaglionati ──
        Column {
            id: infoPanel
            visible: cardOverlay.ok
            anchors.right: parent.right; anchors.rightMargin: parent.width * 0.06
            anchors.verticalCenter: parent.verticalCenter; anchors.verticalCenterOffset: parent.height * 0.06
            width: parent.width * 0.30; spacing: 14
            opacity: cardOverlay.seg(0.22, 0.7)
            transform: Translate { x: 40 * (1 - cardOverlay.seg(0.22, 0.7)) }

            // i campi HamQTH come "righe-dato" che entrano scaglionate (Repeater: i
            // delegate vedono cardOverlay; il model si rivaluta quando callCard cambia)
            Repeater {
                model: [
                    { k: "OPERATORE", v: cardOverlay.c.name || "", at: 0.30 },
                    { k: "QTH",       v: cardOverlay.c.qth || cardOverlay.c.city || "", at: 0.38 },
                    { k: "LOCATORE",  v: cardOverlay.c.grid || "", at: 0.46 },
                    { k: "QSL VIA",   v: cardOverlay.c.qsl || "", at: 0.54 },
                    { k: "ZONE",      v: "ITU " + (cardOverlay.c.itu || "-") + "   ·   CQ " + (cardOverlay.c.cq || "-"), at: 0.62 }
                ]
                delegate: Column {
                    required property var modelData
                    width: infoPanel.width; spacing: 3
                    opacity: cardOverlay.seg(modelData.at, modelData.at + 0.35)
                    Text { text: modelData.k; color: cardOverlay.hud; opacity: 0.65
                           font.pixelSize: 11; font.family: "Consolas"; font.letterSpacing: 3 }
                    Text { text: modelData.v.length ? modelData.v : "—"; color: "#eaf6fb"
                           font.pixelSize: 17; width: infoPanel.width; elide: Text.ElideRight }
                    Rectangle { height: 1; color: cardOverlay.hud; opacity: 0.25
                                width: infoPanel.width * cardOverlay.seg(modelData.at, modelData.at + 0.4) }
                }
            }
        }

        // pulsante chiudi (X) in alto a destra
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 30
            width: 34; height: 34; radius: 17
            color: cl.containsMouse ? "#ff5d72" : Qt.rgba(0.1, 0.16, 0.2, 0.8)
            border.color: cardOverlay.hud; border.width: 1
            opacity: cardOverlay.seg(0.1, 0.5)
            Text { anchors.centerIn: parent; text: "✕"; color: "#eaf6fb"; font.pixelSize: 15 }
            MouseArea { id: cl; anchors.fill: parent; hoverEnabled: true; onClicked: assistant.hideCard() }
        }
        // footer
        Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 22
               visible: cardOverlay.ok; opacity: 0.5 * cardOverlay.seg(0.5, 1.0)
               text: "DATI · HamQTH.com     |     ESC / clic per chiudere"
               color: cardOverlay.hud; font.pixelSize: 11; font.family: "Consolas"; font.letterSpacing: 2 }
    }

    // ───────── SCHEDA PROPAGAZIONE — HUD meteo spaziale, stile Jarvis ─────────
    // Dati live da hamqsl.com (N0NBH): SFI/A/K, X-ray, vento solare, aurora e
    // condizioni di banda HF giorno/notte. Stesse animazioni della scheda stazione.
    Item {
        id: propOverlay
        anchors.fill: parent
        z: 101
        visible: assistant.propVisible || reveal > 0.01
        readonly property var p: assistant.propCard
        readonly property bool ok: p.loading !== true && p.error === undefined
        readonly property color hud: root.accent
        property real reveal: 0
        function seg(a, b) { return Math.max(0, Math.min(1, (reveal - a) / (b - a))) }
        // colori per severità (verde ok · ambra attenzione · rosso scarso)
        function kColor(k)   { var v = parseInt(k);  return isNaN(v) ? hud : (v <= 2 ? "#3dffa0" : v <= 4 ? "#ffb02e" : "#ff5d72") }
        function sfiColor(s) { var v = parseInt(s);  return isNaN(v) ? hud : (v >= 120 ? "#3dffa0" : v >= 90 ? "#ffb02e" : "#ff8a5d") }
        function condColor(c){ c = (c || "").toLowerCase()
                               return c.indexOf("good") >= 0 ? "#3dffa0" : c.indexOf("fair") >= 0 ? "#ffb02e"
                                    : c.indexOf("poor") >= 0 ? "#ff5d72" : "#6f8a98" }

        states: State { name: "on"; when: assistant.propVisible
                        PropertyChanges { propOverlay.reveal: 1 } }
        transitions: [
            Transition { to: "on";   NumberAnimation { property: "reveal"; duration: 540; easing.type: Easing.OutCubic } },
            Transition { from: "on"; NumberAnimation { property: "reveal"; duration: 320; easing.type: Easing.InCubic } }
        ]

        Rectangle { anchors.fill: parent; color: Qt.rgba(0.01, 0.03, 0.06, 0.88 * propOverlay.reveal)
                    MouseArea { anchors.fill: parent; onClicked: assistant.hidePropagation() } }
        Rectangle { width: parent.width; height: 2; color: propOverlay.hud
                    opacity: 0.5 * propOverlay.seg(0.0, 0.5) * (1 - propOverlay.seg(0.5, 1.0))
                    y: parent.height * propOverlay.seg(0.0, 0.85) }

        // staffe angolari
        Repeater {
            model: [[1,1],[-1,1],[1,-1],[-1,-1]]
            delegate: Item {
                required property var modelData
                readonly property bool lft: modelData[0] > 0
                readonly property bool tp:  modelData[1] > 0
                width: 46; height: 46
                x: lft ? 26 : propOverlay.width - 26 - width
                y: tp  ? 26 : propOverlay.height - 26 - height
                opacity: propOverlay.seg(0.0, 0.5)
                Rectangle { height: 3; color: propOverlay.hud; width: 46 * propOverlay.seg(0.05, 0.55)
                            x: parent.lft ? 0 : parent.width - width; y: parent.tp ? 0 : parent.height - 3 }
                Rectangle { width: 3; color: propOverlay.hud; height: 46 * propOverlay.seg(0.05, 0.55)
                            x: parent.lft ? 0 : parent.width - 3; y: parent.tp ? 0 : parent.height - height }
            }
        }

        // intestazione
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * 0.035; spacing: 4
            opacity: propOverlay.seg(0.0, 0.45)
            transform: Translate { y: -22 * (1 - propOverlay.seg(0.0, 0.45)) }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   text: "// METEO  SPAZIALE"; color: propOverlay.hud; opacity: 0.7
                   font.pixelSize: 13; font.family: "Consolas"; font.letterSpacing: 6 }
            Text { anchors.horizontalCenter: parent.horizontalCenter
                   text: "PROPAGAZIONE"; color: propOverlay.hud
                   font.pixelSize: 64; font.bold: true; font.family: "Consolas"; font.letterSpacing: 6
                   layer.enabled: true
                   layer.effect: MultiEffect { shadowEnabled: true; shadowColor: propOverlay.hud
                                               shadowBlur: 1.0; shadowVerticalOffset: 0; shadowHorizontalOffset: 0
                                               autoPaddingEnabled: true } }
            Text { anchors.horizontalCenter: parent.horizontalCenter; visible: propOverlay.ok
                   text: propOverlay.p.geomag ? "Campo geomagnetico: " + propOverlay.p.geomag
                                              + (propOverlay.p.muf ? "   ·   MUF " + propOverlay.p.muf : "") : ""
                   color: "#dff1f8"; font.pixelSize: 16; font.letterSpacing: 1 }
        }

        // stato (scansione / errore)
        Column {
            anchors.centerIn: parent; spacing: 10
            opacity: propOverlay.seg(0.2, 0.6); visible: !propOverlay.ok
            Text { anchors.horizontalCenter: parent.horizontalCenter; visible: propOverlay.p.loading === true
                   text: "◌ LETTURA METEO SPAZIALE…"; color: propOverlay.hud
                   font.pixelSize: 22; font.family: "Consolas"; font.letterSpacing: 3
                   SequentialAnimation on opacity { loops: Animation.Infinite; running: propOverlay.p.loading === true
                       NumberAnimation { to: 0.35; duration: 600 } NumberAnimation { to: 1.0; duration: 600 } } }
            Text { anchors.horizontalCenter: parent.horizontalCenter; visible: propOverlay.p.error !== undefined
                   text: "⚠  " + (propOverlay.p.error || ""); color: "#ffb02e"; font.pixelSize: 18 }
        }

        // ── SOLE con TEXTURE SOLARE REALE: superficie che ruota (scroll) + limb darkening
        // (bordo scuro) = forma sferica + corona pulsante. Texture: solarsystemscope (CC-BY). ──
        Item {
            id: sunBox
            visible: propOverlay.ok
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * 0.15; width: 210; height: 214
            opacity: propOverlay.seg(0.1, 0.55)
            scale: 0.85 + 0.15 * propOverlay.seg(0.1, 0.55)
            // flusso normalizzato 0..1 da SFI (70..200) -> pilota la corona
            property real flux: {
                var v = parseInt(propOverlay.p.sfi)
                return isNaN(v) ? 0.4 : Math.max(0, Math.min(1, (v - 70) / 130))
            }
            Canvas {
                id: sunCanvas
                anchors.horizontalCenter: parent.horizontalCenter; y: 0
                width: 200; height: 200
                property real t: 0
                property bool ready: false
                Component.onCompleted: loadImage("sun.jpg")
                onImageLoaded: { ready = true; requestPaint() }
                Timer { interval: 40; running: propOverlay.visible && sunBox.visible; repeat: true
                        onTriggered: { sunCanvas.t += 0.0016; sunCanvas.requestPaint() } }
                onPaint: {
                    var ctx = getContext("2d")
                    var w = width, h = height, cx = w / 2, cy = h / 2, R = 72, t = sunCanvas.t, fx = sunBox.flux
                    ctx.reset()
                    // corona radiale, pulsa col flusso solare
                    var cR = R + 20 + 6 * Math.sin(t * 60)
                    var cg = ctx.createRadialGradient(cx, cy, R * 0.72, cx, cy, cR)
                    cg.addColorStop(0, "rgba(255,180,50," + (0.4 + 0.28 * fx) + ")")
                    cg.addColorStop(1, "rgba(255,110,20,0)")
                    ctx.fillStyle = cg; ctx.beginPath(); ctx.arc(cx, cy, cR, 0, 2 * Math.PI); ctx.fill()
                    // SUPERFICIE: texture solare reale scrollata orizzontalmente = rotazione
                    ctx.save()
                    ctx.beginPath(); ctx.arc(cx, cy, R, 0, 2 * Math.PI); ctx.clip()
                    if (sunCanvas.ready) {
                        var tw = R * 2.6, th = R * 2.2, off = (t % 1) * tw
                        ctx.drawImage("sun.jpg", cx - R * 1.3 - off,      cy - th / 2, tw, th)
                        ctx.drawImage("sun.jpg", cx - R * 1.3 - off + tw, cy - th / 2, tw, th)
                    } else {
                        ctx.fillStyle = "#ffb02e"; ctx.fillRect(cx - R, cy - R, 2 * R, 2 * R)
                    }
                    // limb darkening: trasparente al centro -> scuro al bordo (sfera)
                    var ld = ctx.createRadialGradient(cx, cy, R * 0.5, cx, cy, R)
                    ld.addColorStop(0, "rgba(0,0,0,0)")
                    ld.addColorStop(0.82, "rgba(70,18,0,0.20)")
                    ld.addColorStop(1, "rgba(35,8,0,0.88)")
                    ctx.fillStyle = ld; ctx.fillRect(cx - R, cy - R, 2 * R, 2 * R)
                    // luce ambient alto-sinistra (dà volume)
                    var hl = ctx.createRadialGradient(cx - R * 0.32, cy - R * 0.32, 2, cx - R * 0.32, cy - R * 0.32, R)
                    hl.addColorStop(0, "rgba(255,250,225,0.28)"); hl.addColorStop(1, "rgba(255,250,225,0)")
                    ctx.fillStyle = hl; ctx.fillRect(cx - R, cy - R, 2 * R, 2 * R)
                    ctx.restore()
                    // limbo luminoso
                    ctx.strokeStyle = "rgba(255,205,90,0.55)"; ctx.lineWidth = 1.5
                    ctx.beginPath(); ctx.arc(cx, cy, R, 0, 2 * Math.PI); ctx.stroke()
                }
            }
            Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.top: sunCanvas.bottom; anchors.topMargin: 6
                   text: "FLUSSO SOLARE · SFI " + (propOverlay.p.sfi || "-")
                   color: propOverlay.hud; font.pixelSize: 13; font.family: "Consolas"; font.letterSpacing: 2 }
        }

        // tiles indicatori
        Grid {
            visible: propOverlay.ok
            anchors.horizontalCenter: parent.horizontalCenter; y: parent.height * 0.45
            columns: 4; rowSpacing: 16; columnSpacing: 16
            opacity: propOverlay.seg(0.15, 0.6)
            transform: Translate { y: 26 * (1 - propOverlay.seg(0.15, 0.6)) }
            Repeater {
                model: [
                    { k: "SFI",        v: propOverlay.p.sfi || "-",                      c: propOverlay.sfiColor(propOverlay.p.sfi) },
                    { k: "MACCHIE",    v: propOverlay.p.sunspots || "-",                 c: propOverlay.hud },
                    { k: "A-INDEX",    v: propOverlay.p.a || "-",                        c: propOverlay.hud },
                    { k: "K-INDEX",    v: propOverlay.p.k || "-",                        c: propOverlay.kColor(propOverlay.p.k) },
                    { k: "RAGGI X",    v: propOverlay.p.xray || "-",                     c: propOverlay.hud },
                    { k: "VENTO SOL.", v: (propOverlay.p.solarwind || "-") + " km/s",    c: propOverlay.hud },
                    { k: "AURORA",     v: propOverlay.p.aurora || "-",                   c: propOverlay.hud },
                    { k: "RUMORE",     v: propOverlay.p.signalnoise || "-",              c: propOverlay.hud }
                ]
                delegate: Rectangle {
                    required property var modelData
                    width: 168; height: 88; radius: 8
                    color: Qt.rgba(0.02, 0.07, 0.11, 0.55); border.color: propOverlay.hud; border.width: 1
                    Column { anchors.centerIn: parent; spacing: 6
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.k
                               color: propOverlay.hud; opacity: 0.7; font.pixelSize: 11; font.family: "Consolas"; font.letterSpacing: 2 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.v
                               color: modelData.c; font.pixelSize: 29; font.bold: true; font.family: "Consolas" } }
                    Rectangle { width: parent.width; height: 2; anchors.bottom: parent.bottom; color: modelData.c; opacity: 0.5 }
                }
            }
        }

        // condizioni di banda HF (giorno/notte)
        Column {
            visible: propOverlay.ok
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom; anchors.bottomMargin: parent.height * 0.10
            spacing: 10
            opacity: propOverlay.seg(0.4, 0.85)
            transform: Translate { y: 28 * (1 - propOverlay.seg(0.4, 0.85)) }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "CONDIZIONI  BANDE  HF"
                   color: propOverlay.hud; opacity: 0.7; font.pixelSize: 12; font.family: "Consolas"; font.letterSpacing: 4 }
            Grid {
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 4; rowSpacing: 10; columnSpacing: 10
                Repeater {
                    model: propOverlay.p.bands || []
                    delegate: Rectangle {
                        required property var modelData
                        width: 150; height: 54; radius: 6
                        color: Qt.rgba(0.02, 0.07, 0.11, 0.5)
                        border.color: propOverlay.condColor(modelData.cond); border.width: 1
                        Column { anchors.centerIn: parent; spacing: 2
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                   text: modelData.band + "   " + (modelData.time === "day" ? "☀ giorno" : "☾ notte")
                                   color: "#cfe6f0"; font.pixelSize: 11; font.family: "Consolas" }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.cond
                                   color: propOverlay.condColor(modelData.cond); font.pixelSize: 16; font.bold: true } }
                    }
                }
            }
        }

        // chiudi + footer
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 30
            width: 34; height: 34; radius: 17
            color: pcl.containsMouse ? "#ff5d72" : Qt.rgba(0.1, 0.16, 0.2, 0.8)
            border.color: propOverlay.hud; border.width: 1
            opacity: propOverlay.seg(0.1, 0.5)
            Text { anchors.centerIn: parent; text: "✕"; color: "#eaf6fb"; font.pixelSize: 15 }
            MouseArea { id: pcl; anchors.fill: parent; hoverEnabled: true; onClicked: assistant.hidePropagation() }
        }
        Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 22
               visible: propOverlay.ok; opacity: 0.5 * propOverlay.seg(0.5, 1.0)
               text: "DATI · hamqsl.com (N0NBH)" + (propOverlay.p.updated ? "   ·   agg. " + propOverlay.p.updated : "")
                     + "     |     clic per chiudere"
               color: propOverlay.hud; font.pixelSize: 11; font.family: "Consolas"; font.letterSpacing: 2 }
    }

    // ───────── SCHEDA DX CLUSTER — HUD spot live, stile Jarvis ─────────
    // Spot DX live da dxwatch.com come lista a cascata, con banda colorata.
    Item {
        id: clusterOverlay
        anchors.fill: parent
        z: 102
        visible: assistant.clusterVisible || reveal > 0.01
        readonly property var c: assistant.clusterCard
        readonly property bool ok: c.loading !== true && c.error === undefined
        readonly property color hud: root.accent
        property real reveal: 0
        function seg(a, b) { return Math.max(0, Math.min(1, (reveal - a) / (b - a))) }
        function bandColor(b) {
            switch (b) {
            case "160m": case "80m": return "#8a7bff"
            case "60m": case "40m": return "#36b6e0"
            case "30m": case "20m": return "#3dffa0"
            case "17m": case "15m": return "#ffd24a"
            case "12m": case "10m": return "#ff8a3d"
            case "6m": case "4m": case "2m": case "70cm": return "#ff5d72"
            default: return hud
            }
        }
        states: State { name: "on"; when: assistant.clusterVisible
                        PropertyChanges { clusterOverlay.reveal: 1 } }
        transitions: [
            Transition { to: "on";   NumberAnimation { property: "reveal"; duration: 540; easing.type: Easing.OutCubic } },
            Transition { from: "on"; NumberAnimation { property: "reveal"; duration: 320; easing.type: Easing.InCubic } }
        ]

        Rectangle { anchors.fill: parent; color: Qt.rgba(0.01, 0.03, 0.06, 0.9 * clusterOverlay.reveal)
                    MouseArea { anchors.fill: parent; onClicked: assistant.hideCluster() } }
        Rectangle { width: parent.width; height: 2; color: clusterOverlay.hud
                    opacity: 0.5 * clusterOverlay.seg(0.0, 0.5) * (1 - clusterOverlay.seg(0.5, 1.0))
                    y: parent.height * clusterOverlay.seg(0.0, 0.85) }

        Repeater {
            model: [[1,1],[-1,1],[1,-1],[-1,-1]]
            delegate: Item {
                required property var modelData
                readonly property bool lft: modelData[0] > 0
                readonly property bool tp:  modelData[1] > 0
                width: 46; height: 46
                x: lft ? 26 : clusterOverlay.width - 26 - width
                y: tp  ? 26 : clusterOverlay.height - 26 - height
                opacity: clusterOverlay.seg(0.0, 0.5)
                Rectangle { height: 3; color: clusterOverlay.hud; width: 46 * clusterOverlay.seg(0.05, 0.55)
                            x: parent.lft ? 0 : parent.width - width; y: parent.tp ? 0 : parent.height - 3 }
                Rectangle { width: 3; color: clusterOverlay.hud; height: 46 * clusterOverlay.seg(0.05, 0.55)
                            x: parent.lft ? 0 : parent.width - 3; y: parent.tp ? 0 : parent.height - height }
            }
        }

        // intestazione
        Column {
            anchors.horizontalCenter: parent.horizontalCenter; y: parent.height * 0.06; spacing: 4
            opacity: clusterOverlay.seg(0.0, 0.45)
            transform: Translate { y: -22 * (1 - clusterOverlay.seg(0.0, 0.45)) }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "// DX  CLUSTER  ·  SPOT  LIVE"
                   color: clusterOverlay.hud; opacity: 0.7; font.pixelSize: 13; font.family: "Consolas"; font.letterSpacing: 6 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "DX CLUSTER"; color: clusterOverlay.hud
                   font.pixelSize: 60; font.bold: true; font.family: "Consolas"; font.letterSpacing: 6
                   layer.enabled: true
                   layer.effect: MultiEffect { shadowEnabled: true; shadowColor: clusterOverlay.hud
                                               shadowBlur: 1.0; shadowVerticalOffset: 0; shadowHorizontalOffset: 0
                                               autoPaddingEnabled: true } }
            Text { anchors.horizontalCenter: parent.horizontalCenter; visible: clusterOverlay.ok
                   text: (clusterOverlay.c.spots ? clusterOverlay.c.spots.length : 0) + " spot recenti"
                   color: "#dff1f8"; font.pixelSize: 15; font.letterSpacing: 1 }
        }

        // stato (interrogazione / errore)
        Column {
            anchors.centerIn: parent; spacing: 10; opacity: clusterOverlay.seg(0.2, 0.6); visible: !clusterOverlay.ok
            Text { anchors.horizontalCenter: parent.horizontalCenter; visible: clusterOverlay.c.loading === true
                   text: "◌ INTERROGAZIONE CLUSTER…"; color: clusterOverlay.hud
                   font.pixelSize: 22; font.family: "Consolas"; font.letterSpacing: 3
                   SequentialAnimation on opacity { loops: Animation.Infinite; running: clusterOverlay.c.loading === true
                       NumberAnimation { to: 0.35; duration: 600 } NumberAnimation { to: 1.0; duration: 600 } } }
            Text { anchors.horizontalCenter: parent.horizontalCenter; visible: clusterOverlay.c.error !== undefined
                   text: "⚠  " + (clusterOverlay.c.error || ""); color: "#ffb02e"; font.pixelSize: 18 }
        }

        // lista spot (scrollabile)
        Item {
            id: spotList
            visible: clusterOverlay.ok
            anchors.horizontalCenter: parent.horizontalCenter; y: parent.height * 0.25
            width: Math.min(parent.width - 120, 880); height: parent.height * 0.63
            opacity: clusterOverlay.seg(0.12, 0.5)
            // intestazione colonne (fissa, allineata alle righe)
            Column {
                id: spotHead
                width: parent.width
                Row { x: 6; width: parent.width - 16; spacing: 0
                    Text { width: 70;  text: "BANDA";    color: clusterOverlay.hud; opacity: 0.55; font.pixelSize: 10; font.family: "Consolas"; font.letterSpacing: 1 }
                    Text { width: 150; text: "DX";       color: clusterOverlay.hud; opacity: 0.55; font.pixelSize: 10; font.family: "Consolas"; font.letterSpacing: 1 }
                    Text { width: 110; text: "FREQ kHz"; color: clusterOverlay.hud; opacity: 0.55; font.pixelSize: 10; font.family: "Consolas"; font.letterSpacing: 1 }
                    Text { width: parent.width - 530; text: "INFO"; color: clusterOverlay.hud; opacity: 0.55; font.pixelSize: 10; font.family: "Consolas"; font.letterSpacing: 1 }
                    Text { width: 200; text: "DE · ORA"; horizontalAlignment: Text.AlignRight; color: clusterOverlay.hud; opacity: 0.55; font.pixelSize: 10; font.family: "Consolas"; font.letterSpacing: 1 }
                }
                Rectangle { x: 6; width: parent.width - 16; height: 1; color: clusterOverlay.hud; opacity: 0.25 }
            }
            // lista scorrevole con tutti gli spot
            ListView {
                id: spotView
                anchors.top: spotHead.bottom; anchors.topMargin: 6
                width: parent.width; height: parent.height - spotHead.height - 6
                clip: true; spacing: 6
                model: clusterOverlay.c.spots || []
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    width: 6; policy: ScrollBar.AsNeeded
                    contentItem: Rectangle { radius: 3; color: clusterOverlay.hud; opacity: 0.45 }
                }
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: spotView.width - 8; height: 38; radius: 5
                    color: index % 2 === 0 ? Qt.rgba(0.05, 0.11, 0.15, 0.55) : Qt.rgba(0.02, 0.06, 0.09, 0.5)
                    // entrata scaglionata, limitata a 12 righe (le successive arrivano insieme)
                    property real st: clusterOverlay.seg(0.16 + Math.min(index, 12) * 0.028, 0.46 + Math.min(index, 12) * 0.028)
                    opacity: st
                    transform: Translate { x: 30 * (1 - st) }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 6
                        width: parent.width - 16; spacing: 0
                        Item { width: 70; height: 26
                            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 56; height: 22; radius: 4
                                color: "transparent"; border.color: clusterOverlay.bandColor(modelData.band); border.width: 1.5
                                Text { anchors.centerIn: parent; text: modelData.band || "?"; color: clusterOverlay.bandColor(modelData.band)
                                       font.pixelSize: 12; font.bold: true; font.family: "Consolas" } } }
                        Text { width: 150; anchors.verticalCenter: parent.verticalCenter; text: modelData.dxcall
                               color: "#ffffff"; font.pixelSize: 18; font.bold: true; font.family: "Consolas"; font.letterSpacing: 1 }
                        Text { width: 110; anchors.verticalCenter: parent.verticalCenter; text: modelData.freq
                               color: clusterOverlay.hud; font.pixelSize: 15; font.family: "Consolas" }
                        Text { width: parent.width - 530; anchors.verticalCenter: parent.verticalCenter
                               text: modelData.info; color: "#cfe6f0"; font.pixelSize: 13; elide: Text.ElideRight }
                        Text { width: 200; anchors.verticalCenter: parent.verticalCenter; horizontalAlignment: Text.AlignRight
                               text: (modelData.spotter ? "de " + modelData.spotter : "") + (modelData.time ? "  " + modelData.time : "")
                               color: "#7fb3c8"; font.pixelSize: 12; font.family: "Consolas"; elide: Text.ElideLeft }
                    }
                }
            }
        }

        // chiudi + footer
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 30; width: 34; height: 34; radius: 17
            color: ccl.containsMouse ? "#ff5d72" : Qt.rgba(0.1, 0.16, 0.2, 0.8); border.color: clusterOverlay.hud; border.width: 1
            opacity: clusterOverlay.seg(0.1, 0.5)
            Text { anchors.centerIn: parent; text: "✕"; color: "#eaf6fb"; font.pixelSize: 15 }
            MouseArea { id: ccl; anchors.fill: parent; hoverEnabled: true; onClicked: assistant.hideCluster() }
        }
        Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 22
               visible: clusterOverlay.ok; opacity: 0.5 * clusterOverlay.seg(0.5, 1.0)
               text: "DATI · dxwatch.com     |     clic per chiudere"
               color: clusterOverlay.hud; font.pixelSize: 11; font.family: "Consolas"; font.letterSpacing: 2 }
    }
}
