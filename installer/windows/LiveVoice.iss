; LiveVoice Windows installer (Inno Setup 6). Built by GitHub Actions:
;   ISCC.exe /DAppVersion=0.7.2 /DSourceVst3=<path to LiveVoice.vst3 folder> /DOutputDir=<dir> installer\windows\LiveVoice.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6F2E4B1C-9A3D-4E7B-8C21-5D0A7F3B9E64}
AppName=LiveVoice
AppVersion={#AppVersion}
AppVerName=LiveVoice {#AppVersion}
AppPublisher=LiveVoice Audio
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
UsePreviousAppDir=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=LiveVoice-Windows-Installer
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=LiveVoice {#AppVersion} (VST3)
CloseApplications=no

[Messages]
WelcomeLabel2=This installs the LiveVoice {#AppVersion} VST3 plug-in into C:\Program Files\Common Files\VST3.%n%nOlder LiveVoice copies there are removed first, so your host always loads this version.%n%nClose LiveProfessor or any other host before continuing, then rescan plug-ins afterwards.

[InstallDelete]
; old copies, including Explorer duplicates such as "LiveVoice.vst3 - Copy" or "LiveVoice 2.vst3"
Type: filesandordirs; Name: "{commoncf64}\VST3\LiveVoice.vst3"
Type: filesandordirs; Name: "{commoncf64}\VST3\LiveVoice*.vst3"

[Files]
Source: "{#SourceVst3}\*"; DestDir: "{commoncf64}\VST3\LiveVoice.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\LiveVoice.vst3"
