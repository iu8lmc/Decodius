; ============================================================================
;  Decodius - Inno Setup script (installer pubblico)
;  Compila con:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" decodius.iss
; ============================================================================
#define MyAppName "Decodius"
; Tieni allineato a DECODIUS_VERSION in CMakeLists.txt (versione mostrata nella UI).
#define MyAppVersion "1.19"
#define MyAppPublisher "IU8LMC"
#define MyAppExe "decodius.exe"
#define Build "C:\Users\IU8LMC\decodius\build"

[Setup]
AppId={{8F2A6C10-3D44-4E9B-9A1C-DEC0D1051A01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppComments=Assistente vocale radioamatoriale
DefaultDirName={autopf}\Decodius
DefaultGroupName=Decodius
DisableProgramGroupPage=yes
LicenseFile=
InfoBeforeFile={#Build}\LEGGIMI.txt
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName=Decodius
OutputDir=C:\Users\IU8LMC\decodius\installer
OutputBaseFilename=Decodius-Setup-{#MyAppVersion}
SetupIconFile={#Build}\decodius.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Languages]
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; --- Eseguibile + runtime Qt (DLL nella root) + risorse/config ---
Source: "{#Build}\{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\decodius.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\decodius_system.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\decodius_ham_kb.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\decodius_model.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\LEGGIMI.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#Build}\LEGGIMI-Decodium.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\setup_cervello.ps1"; DestDir: "{app}"; Flags: ignoreversion
; --- Plugin Qt e moduli QML (cartelle di deploy di windeployqt) ---
Source: "{#Build}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\multimedia\*"; DestDir: "{app}\multimedia"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\texttospeech\*"; DestDir: "{app}\texttospeech"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\qmltooling\*"; DestDir: "{app}\qmltooling"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\translations\*"; DestDir: "{app}\translations"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\QtQuick\*"; DestDir: "{app}\QtQuick"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\Decodius\*"; DestDir: "{app}\Decodius"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\qml\*"; DestDir: "{app}\qml"; Flags: ignoreversion recursesubdirs createallsubdirs
; --- Voce: server edge + Python PORTATILE con edge-tts ---
Source: "{#Build}\edge\edge_server.py"; DestDir: "{app}\edge"; Flags: ignoreversion
Source: "{#Build}\pyedge\*"; DestDir: "{app}\pyedge"; Flags: ignoreversion recursesubdirs createallsubdirs
; --- STT (comandi vocali): OPZIONALE, scaricato a richiesta (installer leggero) ---
; Lo STT non è impacchettato (sarebbero ~700 MB): setup_stt.ps1 scarica a richiesta
; un Python portatile con faster-whisper + modello 'small' in {app}\pywhisper.
; Attivabile dal task post-install opzionale o dall'icona "Attiva voce (STT)".
Source: "{#Build}\setup_stt.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\whisper_server.py"; DestDir: "{app}"; Flags: ignoreversion
; --- MCP (tool esterni, OPZIONALE): ponte + esempio + config di esempio. Si attiva
; copiando decodius_mcp.example.json in decodius_mcp.json e configurando i server. ---
Source: "{#Build}\mcp_bridge.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\mcp_example_server.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\mcp_station_server.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\decodius_mcp.example.json"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Decodius"; Filename: "{app}\{#MyAppExe}"; IconFilename: "{app}\decodius.ico"; WorkingDir: "{app}"
Name: "{group}\Configura cervello (Ollama)"; Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\setup_cervello.ps1"""; IconFilename: "{app}\decodius.ico"; WorkingDir: "{app}"
Name: "{group}\Attiva voce (STT)"; Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\setup_stt.ps1"""; IconFilename: "{app}\decodius.ico"; WorkingDir: "{app}"
Name: "{group}\Leggimi"; Filename: "{app}\LEGGIMI.txt"
Name: "{group}\Collega Decodium"; Filename: "{app}\LEGGIMI-Decodium.txt"
Name: "{group}\Disinstalla Decodius"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Decodius"; Filename: "{app}\{#MyAppExe}"; IconFilename: "{app}\decodius.ico"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\setup_cervello.ps1"""; Description: "Configura ora il cervello (Ollama + qwen3:1.7b, locale) — consigliato"; WorkingDir: "{app}"; Flags: postinstall skipifsilent runasoriginaluser
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\setup_stt.ps1"""; Description: "Attiva i comandi vocali (STT, ~700 MB, opzionale)"; WorkingDir: "{app}"; Flags: postinstall skipifsilent unchecked
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,Decodius}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; rimuove il config del nominativo alla disinstallazione
Type: filesandordirs; Name: "{userappdata}\Decodius"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  MsgBox('Decodius usa OLLAMA come "cervello".' + #13#10 + #13#10 +
         'NON devi configurare nulla a mano: al termine dell''installazione' + #13#10 +
         'lascia spuntata l''opzione "Configura ora il cervello".' + #13#10 +
         'Penserà a tutto lei: installa Ollama e scarica il modello' + #13#10 +
         'locale qwen3:1.7b (gira sul tuo PC).' + #13#10 + #13#10 +
         'Serve solo Internet per il primo download (~1.4 GB). Niente account.', mbInformation, MB_OK);
end;
