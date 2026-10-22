; LifeScreen Recorder - Modern Windows Installer Script
; Styled for LifeScreen by Baneronetwo (BANSAFAn)

#define MyAppName "LifeScreen Recorder"
#define MyAppVersion "0.1.1"
#define MyAppPublisher "Baneronetwo (BANSAFAn)"
#define MyAppURL "https://github.com/lifescreen-recorder/Code"
#define MyAppExeName "lifescreen.exe"

[Setup]
AppId={{E68BC208-1122-4D38-958C-5D9F8A4F28F5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\LifeScreen
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableReadyPage=yes
AllowNoIcons=yes
OutputDir=..\dist
OutputBaseFilename=lifescreen-windows-x64-installer
SetupIconFile=..\resources\icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

; High-res sleek branding graphics
WizardImageFile=..\resources\installer_banner.bmp
WizardSmallImageFile=..\resources\installer_small.bmp

; Modern compression
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardResizable=no
WizardSizePercent=105,105
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; Authenticode and Windows Explorer Version Properties
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=LifeScreen Recorder Installer
VersionInfoVersion={#MyAppVersion}.0
VersionInfoCopyright=Copyright (C) 2026 {#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoOriginalFileName=lifescreen-windows-x64-installer.exe

[Languages]
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\zig-out\bin\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
