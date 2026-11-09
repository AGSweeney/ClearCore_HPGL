; ClearHPGL: Windows installer (Inno Setup 6).
; Built by installer\build_installer.ps1, which stages the deployed app, the firmware and the documents in
; StageDir and passes the version from Firmware\ClearCoreFirmware\PlotterConfig.h (the app and the firmware
; ship together). Output: dist\ClearHPGL-<version>-setup.exe.

#define AppName "ClearHPGL"
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\ProjectData\build\installer_stage"
#endif

[Setup]
AppId={{6C03410F-339E-40A1-B4B5-9450974CA266}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Adam G. Sweeney
AppPublisherURL=https://github.com/AGSweeney/ClearCore_HPGL
AppSupportURL=https://github.com/AGSweeney/ClearCore_HPGL
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\ClearHPGL
DefaultGroupName=ClearHPGL
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=ClearHPGL-{#AppVersion}-setup
SetupIconFile=..\resources\icons\clearcnc_app_icon.ico
UninstallDisplayIcon={app}\ClearHPGL.exe
UninstallDisplayName={#AppName} {#AppVersion}
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The app with its Qt and MSVC runtime (windeployqt output, test programs left out), and in firmware\ the
; ClearHPGL firmware and bossac for Plotter > Update board firmware
Source: "{#StageDir}\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; The command-line flasher next to the firmware
Source: "{#StageDir}\flash\*"; DestDir: "{app}\firmware"; Flags: ignoreversion recursesubdirs createallsubdirs
; Documents and licences
Source: "{#StageDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\ClearHPGL.exe"
Name: "{group}\ClearHPGL User's Manual"; Filename: "{app}\docs\clearhpgl-manual.pdf"
Name: "{group}\Flash the ClearCore firmware (USB)"; Filename: "{app}\firmware\flash_firmware.cmd"; WorkingDir: "{app}\firmware"
Name: "{group}\Firmware files"; Filename: "{app}\firmware"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\ClearHPGL.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ClearHPGL.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
