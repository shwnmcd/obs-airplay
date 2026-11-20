; Optional Inno Setup 6 script for OBS AirPlay Screen Cast (Windows x64)
#define MyAppName "OBS AirPlay Screen Cast"
#define MyAppVersion "1.2.1"
#define MyAppPublisher "obs-airplay"

[Setup]
AppId={{8E9A74B1-52C4-4E12-98C4-77D521B942A1}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={commonappdata}\obs-studio\plugins\obs-airplay
DisableDirPage=no
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=obs-airplay-{#MyAppVersion}-windows-x64-InnoSetup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
CloseApplications=yes

[Files]
Source: "..\build\obs-airplay.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "..\data\airplay_frame.effect"; DestDir: "{app}\data"; Flags: ignoreversion
Source: "..\data\locale\en-US.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "firewall"; Description: "Configure Windows Firewall rules for local Wi-Fi AirPlay (LocalSubnet only)"; Flags: checkedonce

[Run]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""OBS AirPlay Mirroring (TCP)"""; Flags: runhidden; Tasks: firewall
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""OBS AirPlay Mirroring (UDP)"""; Flags: runhidden; Tasks: firewall
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""OBS AirPlay Mirroring (TCP)"" dir=in action=allow program=""C:\Program Files\obs-studio\bin\64bit\obs64.exe"" protocol=TCP localport=7000,7001,7100 remoteip=localsubnet profile=any enable=yes"; Flags: runhidden; Tasks: firewall
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""OBS AirPlay Mirroring (UDP)"" dir=in action=allow program=""C:\Program Files\obs-studio\bin\64bit\obs64.exe"" protocol=UDP localport=5353,6000,6001,7011 remoteip=localsubnet profile=any enable=yes"; Flags: runhidden; Tasks: firewall

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""OBS AirPlay Mirroring (TCP)"""; Flags: runhidden
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""OBS AirPlay Mirroring (UDP)"""; Flags: runhidden
