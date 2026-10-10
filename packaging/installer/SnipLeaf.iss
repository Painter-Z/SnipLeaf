#ifndef StageDir
  #error StageDir is required
#endif
#define ProductName "SnipLeaf"
#ifndef ProductVersion
  #error ProductVersion is required
#endif

[Setup]
AppId={{B89FD0F4-6E5C-4E25-BC7E-E443B31D6592}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppVerName={#ProductName} {#ProductVersion} 试用版
AppPublisher=SnipLeaf Project
DefaultDirName={localappdata}\Programs\SnipLeaf
DefaultGroupName={#ProductName}
PrivilegesRequired=lowest
SetupArchitecture=x64
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
WizardStyle=modern
WizardSizePercent=110
DisableProgramGroupPage=yes
DisableWelcomePage=no
InfoBeforeFile=Preview.txt
OutputDir=..\..\out\release
OutputBaseFilename=SnipLeaf-{#ProductVersion}-x64-Preview-Setup
SetupIconFile=..\..\resources\SnapLite.ico
UninstallDisplayIcon={app}\SnipLeaf.exe
Compression=lzma2
SolidCompression=yes
AppMutex=Local\SnapLite.Desktop.Singleton
CloseApplications=no
RestartApplications=no
Uninstallable=yes
UsePreviousAppDir=yes
UninstallLogging=yes
VersionInfoDescription=SnipLeaf 朋友试用版安装程序

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："
Name: "startup"; Description: "登录 Windows 后自动启动"; GroupDescription: "启动："; Flags: unchecked

[Files]
Source: "{#StageDir}\SnipLeaf.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\SnipLeaf.identity.msix"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\SnipLeaf-preview.cer"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\installer-security.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\Assets\*"; DestDir: "{app}\Assets"; Flags: ignoreversion
Source: "Deploy-App.ps1"; DestDir: "{app}\Installer"; Flags: ignoreversion
Source: "Trust-PreviewCertificate.ps1"; DestDir: "{app}\Installer"; Flags: ignoreversion
Source: "Initialize-OcrLanguages.ps1"; DestDir: "{app}\Installer"; Flags: ignoreversion
Source: "{#StageDir}\使用说明.txt"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: RegisterApplication

[Icons]
Name: "{group}\{#ProductName}"; Filename: "{app}\SnipLeaf.exe"; WorkingDir: "{app}"
Name: "{group}\卸载 SnipLeaf"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#ProductName}"; Filename: "{app}\SnipLeaf.exe"; Tasks: desktopicon
Name: "{userstartup}\{#ProductName}"; Filename: "{app}\SnipLeaf.exe"; Tasks: startup

[Run]
Filename: "{app}\SnipLeaf.exe"; Description: "启动 SnipLeaf"; Flags: nowait postinstall skipifsilent runasoriginaluser
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -File ""{app}\Installer\Initialize-OcrLanguages.ps1"" -ReportDirectory ""{localappdata}\SnipLeaf\Diagnostics"" -Interactive"; Verb: "runas"; Description: "补齐中英文 OCR 语言（联网、管理员确认，在独立窗口显示进度）"; Flags: shellexec postinstall skipifsilent unchecked; Check: NeedsOcrSetup

[UninstallDelete]
Type: files; Name: "{app}\installation-state.json"
Type: files; Name: "{app}\installation-check.json"
Type: files; Name: "{app}\ocr-prerequisites.json"
Type: files; Name: "{app}\restart-required.flag"
Type: files; Name: "{app}\ocr-setup-needed.flag"
Type: files; Name: "{app}\ocr-unavailable.flag"

[Code]
var
  ConsentPage: TInputOptionWizardPage;
  RegistrationFailed: Boolean;
  OcrNoticeShown: Boolean;

function NeedsOcrSetup: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\ocr-setup-needed.flag'));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and NeedsOcrSetup and not OcrNoticeShown then begin
    OcrNoticeShown := True;
    if FileExists(ExpandConstant('{app}\ocr-unavailable.flag')) then
      WizardForm.FinishedLabel.Caption := 'SnipLeaf 已安装，截图、标注和贴图可使用。当前没有可用的 OCR 语言，文字识别需先补齐 Windows 语言组件。'
    else
      WizardForm.FinishedLabel.Caption := 'SnipLeaf 已安装，已有语言可以识别文字。中英文 OCR 尚未配齐，可勾选下方选项，或稍后在 SnipLeaf 设置中配置。';
  end;
end;

function ExistingInstallation: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B89FD0F4-6E5C-4E25-BC7E-E443B31D6592}_is1');
end;

function InitializeSetup: Boolean;
begin
  Result := not ExistingInstallation;
  if not Result then
    SuppressibleMsgBox('已安装 SnipLeaf。此首版试用包请先卸载旧版再安装，个人设置会保留。', mbInformation, MB_OK, IDOK);
end;

function TrustConsent: Boolean;
begin
  Result := ConsentPage.Values[0] or (ExpandConstant('{param:TRUSTPREVIEW|0}') = '1');
end;

procedure InitializeWizard;
begin
  ConsentPage := CreateInputOptionPage(wpInfoBefore, '启用离线 OCR',
    '此试用版需要信任专用公共证书',
    '安装将为当前用户注册 SnipLeaf 的 Windows OCR 应用身份。仅信任 SnipLeaf Preview 专用证书，不添加根证书；必要时 Windows 会请求管理员确认。卸载会清理本安装添加的信任。',
    False, False);
  ConsentPage.Add('我同意为此试用版信任 SnipLeaf Preview 专用公共证书');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = ConsentPage.ID) and not TrustConsent then begin
    if not WizardSilent then
      MsgBox('启用此试用版 OCR 需要明确同意专用证书信任。您可以取消安装。', mbInformation, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not TrustConsent then begin
    Result := '此试用版需要明确同意专用证书信任，安装未进行任何更改。';
    Exit;
  end;
  if ExistingInstallation then
    Result := '请先卸载已安装的 SnipLeaf 试用版，个人设置会保留。';
end;

procedure RegisterApplication;
var
  ResultCode: Integer;
  Parameters: String;
begin
  Parameters := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    AddQuotes(ExpandConstant('{app}\Installer\Deploy-App.ps1')) + ' -Action Install -Directory ' +
    AddQuotes(ExpandConstant('{app}')) + ' -TrustPreview';
  WizardForm.StatusLabel.Caption := '正在验证应用身份和已有 OCR 语言，可能需要 Windows 管理员确认…';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then begin
    RegistrationFailed := True;
    Log('SnipLeaf identity verification failed. Canceling installation before commit.');
    if not WizardSilent then
      MsgBox('应用身份注册或程序验证失败，安装将回滚。详细部署事件已尽可能记录到 %TEMP%\SnipLeaf-install-last.log，请将日志交给开发者排查。', mbError, MB_OK);
    { Inno ignores exceptions in AfterInstall. Request its own cancellation
      while the transaction is still open, including /VERYSILENT mode. }
    WizardForm.Show;
    WizardForm.Close;
  end;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if RegistrationFailed then begin
    Cancel := True;
    Confirm := False;
  end;
end;

function InitializeUninstall: Boolean;
begin
  Result := False;
  if CheckForMutexes('Local\SnapLite.Desktop.Singleton') then begin
    MsgBox('请先从托盘退出 SnipLeaf，再运行卸载。', mbInformation, MB_OK);
    Exit;
  end;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Parameters: String;
begin
  if CurUninstallStep <> usUninstall then
    Exit;
  Parameters := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    AddQuotes(ExpandConstant('{app}\Installer\Deploy-App.ps1')) + ' -Action Uninstall -Directory ' +
    AddQuotes(ExpandConstant('{app}'));
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then begin
    if not UninstallSilent then
      MsgBox('应用身份或测试证书清理未完成，卸载已停止。请查看 %TEMP%\SnipLeaf-install-last.log 后重试。', mbError, MB_OK);
    Abort;
  end;
end;
