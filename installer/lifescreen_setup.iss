; LifeScreen Recorder - Modern Windows Installer Script
; Styled for LifeScreen by Baneronetwo (BANSAFAn)

#define MyAppName "LifeScreen Recorder"
#define MyAppVersion "0.1.0"
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
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardResizable=no
WizardSizePercent=105,105
DisableWelcomePage=no
DisableFinishedPage=no
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

[Code]
type
  DWMWINDOWATTRIBUTE = DWORD;

function DwmSetWindowAttribute(hwnd: HWND; dwAttribute: DWMWINDOWATTRIBUTE; var pvAttribute: DWORD; cbAttribute: DWORD): Longint;
external 'DwmSetWindowAttribute@dwmapi.dll stdcall';

procedure ApplyDarkTheme();
var
  DarkMode: DWORD;
  DarkBg: TColor;
  CardBg: TColor;
  CyanText: TColor;
  MutedText: TColor;
begin
  DarkMode := 1;
  // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 on Windows 10 (20H1+) and Windows 11
  DwmSetWindowAttribute(WizardForm.Handle, 20, DarkMode, 4);

  // LifeScreen brand palette (Delphi BGR format)
  DarkBg := $140E0B;   // #0B0E14
  CardBg := $1E1511;   // #11151E
  CyanText := $E9A50E; // #0EA5E9
  MutedText := $B8A394;// #94A3B8

  WizardForm.Color := DarkBg;
  WizardForm.Font.Name := 'Segoe UI';

  // Header Panel
  WizardForm.MainPanel.Color := CardBg;
  WizardForm.PageNameLabel.Font.Color := CyanText;
  WizardForm.PageNameLabel.Font.Style := [fsBold];
  WizardForm.PageDescriptionLabel.Font.Color := MutedText;

  // Inner Page
  WizardForm.InnerPage.Color := DarkBg;

  // Welcome Page
  WizardForm.WelcomePage.Color := DarkBg;
  WizardForm.WelcomeLabel1.Font.Color := CyanText;
  WizardForm.WelcomeLabel1.Font.Style := [fsBold];
  WizardForm.WelcomeLabel2.Font.Color := clWhite;

  // Finished Page
  WizardForm.FinishedPage.Color := DarkBg;
  WizardForm.FinishedHeadingLabel.Font.Color := CyanText;
  WizardForm.FinishedHeadingLabel.Font.Style := [fsBold];
  WizardForm.FinishedLabel.Font.Color := clWhite;

  // Directory Selection
  WizardForm.SelectDirLabel.Font.Color := clWhite;
  WizardForm.SelectDirBrowseLabel.Font.Color := MutedText;
  WizardForm.DiskSpaceLabel.Font.Color := MutedText;
  WizardForm.DirEdit.Color := CardBg;
  WizardForm.DirEdit.Font.Color := clWhite;

  // Tasks Selection
  WizardForm.SelectTasksLabel.Font.Color := clWhite;
  WizardForm.TasksList.Color := CardBg;
  WizardForm.TasksList.Font.Color := clWhite;

  // Progress Page
  WizardForm.StatusLabel.Font.Color := CyanText;
  WizardForm.FilenameLabel.Font.Color := MutedText;

  // Clean UI: Remove legacy 3D bevel borders
  WizardForm.Bevel.Visible := False;
  WizardForm.BeveledLabel.Visible := False;
end;

procedure InitializeWizard();
begin
  ApplyDarkTheme();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  ApplyDarkTheme();
end;

