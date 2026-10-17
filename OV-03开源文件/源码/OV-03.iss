; ==================================================================
;  OV-03 安装脚本 (Inno Setup 7)
;  编译(绝对路径): "C:\Program Files\Inno Setup 7\ISCC.exe" "D:\dekstop mainstage\303seq\installer\OV-03.iss"
;  版本规则: v1.0 -> v1.1 ... v1.10 (小改) ; 大版本动前一位 -> v2.0
;  兼容: Win7(含未打SP1) ~ Win11. 缺 .NET 自动装 4.0 (一个版本覆盖全部);
;        装不上也不中断, 由启动器(OV-03-Launcher.exe)在打开时弹窗提示。
; ==================================================================
#define AppName "OV-03"
#define AppVer  "1.2"
#define AppDisplay AppName + " v" + AppVer
#define SrcExe "D:\dekstop mainstage\OV-03-dev\OV-03-dev.exe"
#define PatternsDir "D:\dekstop mainstage\OV-03-dev\pattern"
#define PrintExe "D:\dekstop mainstage\OV-03-dev\303paint.exe"
#define Launcher "D:\dekstop mainstage\303seq\installer\launcher\OV-03-Launcher.exe"
#define OutDir "D:\dekstop mainstage"
#define IconFile "D:\dekstop mainstage\303seq\OV-03.ico"
#define ReadMeFile "D:\dekstop mainstage\303seq\installer\readme\OV-03_ReadMe.txt"
#define DriversDir "D:\dekstop mainstage\303seq\installer\drivers"
#define Net40 "D:\dekstop mainstage\303seq\installer\prereq\dotNetFx40_Full_x86_x64.exe"

[Setup]
AppId={{F8945E90-4AD0-48B0-9F97-9E583E68D059}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppDisplay}
AppPublisher=OV-03
; 程序运行时持有的互斥体: 安装/卸载时若程序在运行会提示先关闭 (避免文件占用)
AppMutex=OV-03_App
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; 始终显示"选择安装位置"页 (Inno 默认升级时隐藏 -> 让人以为不能选路径)
DisableDirPage=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#OutDir}
OutputBaseFilename=OV-03_v{#AppVer}_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppDisplay}
UninstallDisplayIcon={app}\OV-03.exe
SetupIconFile={#IconFile}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "usbdriver"; Description: "安装 USB 串口驱动（仅 Win7/8 旧系统需要）"; GroupDescription: "可选组件："; Flags: unchecked

[Dirs]
; 安装后建数据子目录: pattern=存/读图案, records=录音, log=日志(程序里用绝对路径定位)
Name: "{app}\pattern"
Name: "{app}\records"
Name: "{app}\log"

[Files]
Source: "{#SrcExe}"; DestDir: "{app}"; DestName: "OV-03.exe"; Flags: ignoreversion
Source: "{#Launcher}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReadMeFile}"; DestDir: "{app}"; DestName: "OV-03_ReadMe.txt"; Flags: ignoreversion isreadme
Source: "{#DriversDir}\*"; DestDir: "{app}\drivers"; Flags: ignoreversion recursesubdirs createallsubdirs
; 内置 4 个音序 (.303): 首次打开自动加载 Emotinium.303, 其余手动读
Source: "{#PatternsDir}\*.303"; DestDir: "{app}\pattern"; Flags: ignoreversion
; 默认皮肤 (.303paint) + 背景图 (png)
Source: "D:\dekstop mainstage\303seq\installer\skins\*.303paint"; DestDir: "{app}\skins"; Flags: ignoreversion
Source: "D:\dekstop mainstage\303seq\installer\skins\*.png"; DestDir: "{app}\skins"; Flags: ignoreversion
; 皮肤编辑器 303paint
Source: "{#PrintExe}"; DestDir: "{app}"; Flags: ignoreversion
; ffmpeg (精简版, 供 303paint 导入 GIF/视频时抽帧)
Source: "D:\dekstop mainstage\303seq\installer\tools\ffmpeg.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "D:\dekstop mainstage\303seq\installer\tools\ffprobe.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
; .NET Framework 4.0 离线安装包 (微软官方, 已验签): 不直接安装, 仅供 [Code] 按需提取
Source: "{#Net40}"; Flags: dontcopy

[Icons]
; 快捷方式指向启动器: 打开时先检测 .NET, 没有就弹窗提示
Name: "{group}\{#AppDisplay}"; Filename: "{app}\OV-03-Launcher.exe"
Name: "{group}\303paint 皮肤编辑器"; Filename: "{app}\303paint.exe"
Name: "{group}\安装须知 (ReadMe)"; Filename: "{app}\OV-03_ReadMe.txt"
Name: "{group}\{cm:UninstallProgram,{#AppDisplay}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppDisplay}"; Filename: "{app}\OV-03-Launcher.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\drivers\install_usb_serial.cmd"; Description: "安装 USB 串口驱动（旧系统）"; Flags: shellexec waituntilterminated; Tasks: usbdriver
Filename: "{app}\OV-03-Launcher.exe"; Description: "{cm:LaunchProgram,{#AppDisplay}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 卸载时清掉整个 log 目录(日志/首启标记), 空目录时删 pattern 与安装目录 (pattern 里用户自己存的文件不删)
Type: filesandordirs; Name: "{app}\log"
Type: dirifempty; Name: "{app}\pattern"
Type: dirifempty; Name: "{app}"

[Code]
var
  gNetWarn: Boolean;

{ 是否已装有任意 .NET 4.x (4.0: Install=1 ; 4.5+: Release>=378389) }
function Net4xPresent(): Boolean;
var
  install, release: Cardinal;
begin
  Result := False;
  if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full', 'Install', install) and (install = 1) then
    Result := True
  else if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full', 'Release', release) and (release >= 378389) then
    Result := True;
end;

{ 安装前: 缺 .NET 就提权静默装 4.0 (Win7 等); 失败不中断, 只记下 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  r: Integer;
  exe: String;
begin
  Result := '';
  gNetWarn := False;
  if Net4xPresent() then exit;
  ExtractTemporaryFile('dotNetFx40_Full_x86_x64.exe');
  exe := ExpandConstant('{tmp}\dotNetFx40_Full_x86_x64.exe');
  WizardForm.StatusLabel.Caption := '正在安装 .NET Framework 4.0（首次需要，请稍候）...';
  if not ShellExec('runas', exe, '/q /norestart', '', SW_SHOW, ewWaitUntilTerminated, r) then
    gNetWarn := True
  else if not Net4xPresent() then
    gNetWarn := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and gNetWarn then
    MsgBox('未能自动安装 .NET Framework 4.x。'#13#10 +
           '请参考安装目录中的 OV-03_ReadMe.txt 手动安装，否则程序无法运行。',
           mbInformation, MB_OK);
end;
