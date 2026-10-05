; Multiway Windows installer (Inno Setup 6.3+).
;
; Build after a CMake Release build (cmake -S plugin -B build):
;   iscc installer\Multiway.iss /DAppVersion=1.1
;
; Optional defines:
;   AppVersion  version shown in the installer and in Apps & features (default below)
;   BuildDir    folder that holds the JUCE artefacts (VST3\, Standalone\)
;
; Components:
;   VST3        required, installed to {commoncf64}\VST3
;   Standalone  optional, installed to {commonpf64}\BopsAudio\Multiway with a Start menu shortcut

#ifndef AppVersion
  #define AppVersion "1.1"
#endif

#ifndef BuildDir
  #define BuildDir AddBackslash(SourcePath) + "..\build\Multiway_artefacts\Release"
#endif

; On Windows, JUCE builds the VST3 as a bundle folder (Multiway.vst3\Contents\x86_64-win\Multiway.vst3
; plus Contents\Resources\moduleinfo.json), not as a single .vst3 file. The whole folder is installed.
#define Vst3Bundle BuildDir + "\VST3\Multiway.vst3"
#define StandaloneExe BuildDir + "\Standalone\Multiway.exe"

#if !DirExists(Vst3Bundle)
  #error VST3 bundle folder (VST3\Multiway.vst3) not found in BuildDir. Build Release first, or pass /DBuildDir=...
#endif
#if !FileExists(StandaloneExe)
  #error Standalone app (Standalone\Multiway.exe) not found in BuildDir. Build Release first, or pass /DBuildDir=...
#endif

[Setup]
; Never change AppId: it lets a newer installer upgrade the existing installation.
AppId={{B93C5B8C-15B6-4A4A-A206-1B03107A76C6}
AppName=Multiway
AppVersion={#AppVersion}
AppVerName=Multiway {#AppVersion}
AppPublisher=BopsAudio
AppPublisherURL=https://bopsaudio.com
AppSupportURL=https://github.com/mustafamazi/Multiway
VersionInfoVersion={#AppVersion}
VersionInfoCompany=BopsAudio
VersionInfoProductName=Multiway

DefaultDirName={commonpf64}\BopsAudio\Multiway
DisableDirPage=yes
DefaultGroupName=BopsAudio
DisableProgramGroupPage=yes

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0

WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir=Output
OutputBaseFilename=Multiway-v{#AppVersion}-Windows-Setup
UninstallDisplayName=Multiway
CloseApplications=yes

[Types]
Name: "full";   Description: "VST3 plugin and standalone app"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3";       Description: "VST3 plugin";    Types: full custom; Flags: fixed
Name: "standalone"; Description: "Standalone app"; Types: full

[Files]
Source: "{#Vst3Bundle}\*"; DestDir: "{commoncf64}\VST3\Multiway.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StandaloneExe}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{group}\Multiway"; Filename: "{app}\Multiway.exe"; Components: standalone

[Run]
Filename: "{app}\Multiway.exe"; Description: "Launch Multiway"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked

[UninstallDelete]
; Remove the whole bundle, including anything a host may have left inside it.
Type: filesandordirs; Name: "{commoncf64}\VST3\Multiway.vst3"
