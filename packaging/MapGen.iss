; Установщик MapGen — Inno Setup 6.
; Сборка: ISCC.exe /DAppVersion=1.0.0 /DSourceDir=<папка развёртывания> /DOutputDir=<куда> packaging\MapGen.iss
; CI собирает его на Windows и проверяет: тихая установка, самопроверка установленной программы, удаление.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\MapGen"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{8C1F4A6E-2B7D-4E3A-9F51-7A2D6C0B3E19}
AppName=MapGen
AppVersion={#AppVersion}
AppVerName=MapGen {#AppVersion}
AppPublisher=MapGen
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=MapGen
VersionInfoDescription=MapGen setup
DefaultDirName={autopf}\MapGen
DefaultGroupName=MapGen
DisableProgramGroupPage=yes
; Для текущего пользователя, без прав администратора; в окне установки можно выбрать «для всех».
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=MapGen-{#AppVersion}-Setup
SetupIconFile=mapgen.ico
UninstallDisplayIcon={app}\MapGen.exe
UninstallDisplayName=MapGen {#AppVersion}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
CloseApplications=yes
ChangesAssociations=yes
ShowLanguageDialog=auto

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
russian.SoftwareMode=MapGen (программная отрисовка)
english.SoftwareMode=MapGen (software rendering)
russian.Guide=Руководство пользователя
english.Guide=User guide (Russian)
russian.UninstallShortcut=Удалить MapGen
english.UninstallShortcut=Uninstall MapGen
russian.AssocTask=Открывать файлы проектов .geoworld в MapGen
english.AssocTask=Open .geoworld project files with MapGen

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assoc"; Description: "{cm:AssocTask}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\MapGen"; Filename: "{app}\MapGen.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:SoftwareMode}"; Filename: "{app}\MapGen.exe"; Parameters: "--software-render"; WorkingDir: "{app}"
Name: "{group}\{cm:Guide}"; Filename: "{app}\docs\USER_GUIDE_RU.md"
Name: "{group}\{cm:UninstallShortcut}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\MapGen"; Filename: "{app}\MapGen.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.geoworld"; ValueType: string; ValueName: ""; ValueData: "MapGen.World"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MapGen.World"; ValueType: string; ValueName: ""; ValueData: "Проект MapGen"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MapGen.World\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\MapGen.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MapGen.World\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MapGen.exe"" ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\MapGen.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,MapGen}"; Flags: nowait postinstall skipifsilent
