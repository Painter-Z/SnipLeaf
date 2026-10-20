#ifndef StageDir
  #error StageDir is required
#endif
#ifndef ProductName
  #define ProductName "SnipLeaf"
#endif
#ifndef SetupAppId
  #define SetupAppId "B89FD0F4-6E5C-4E25-BC7E-E443B31D6592"
#endif
#ifndef SetupMutex
  #define SetupMutex "Local\SnapLite.Desktop.Singleton"
#endif
#ifndef ProductVersion
  #error ProductVersion is required
#endif

[Setup]
AppId={{{#SetupAppId}}
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
AppMutex={#SetupMutex}
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
Filename: "{app}\SnipLeaf.exe"; Description: "启动 SnipLeaf（首次启动自动检查并准备缺少的 OCR 语言）"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
Type: files; Name: "{app}\installation-state.json"
Type: files; Name: "{app}\installation-check.json"
Type: files; Name: "{app}\ocr-prerequisites.json"
Type: files; Name: "{app}\restart-required.flag"
Type: files; Name: "{app}\ocr-setup-needed.flag"
Type: files; Name: "{app}\ocr-unavailable.flag"
Type: files; Name: "{app}\ocr-identity-needed.flag"
Type: files; Name: "{app}\application-check-needed.flag"
Type: files; Name: "{app}\deployment-helper-needed.flag"

[Code]
var
  ConsentPage: TInputOptionWizardPage;
  RegistrationFailed: Boolean;
  OcrNoticeShown: Boolean;

function NeedsOcrIdentity: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\ocr-identity-needed.flag')) or
    FileExists(ExpandConstant('{app}\deployment-helper-needed.flag'));
end;

function NeedsOcrSetup: Boolean;
begin
  Result := not NeedsOcrIdentity and FileExists(ExpandConstant('{app}\ocr-setup-needed.flag'));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and not OcrNoticeShown then begin
    OcrNoticeShown := True;
    if FileExists(ExpandConstant('{app}\application-check-needed.flag')) then
      WizardForm.FinishedLabel.Caption := 'SnipLeaf 文件已安装，但程序启动检查未通过。系统或安全软件可能阻止运行，请查看安装日志；本提示不表示软件已经可以正常使用。'
    else if NeedsOcrIdentity then
      WizardForm.FinishedLabel.Caption := 'SnipLeaf 基础功能已安装，OCR 身份尚未就绪。可以先截图、标注和贴图，再从托盘设置中选择“修复 OCR 身份…”。身份准备失败不会撤销基础安装。'
    else if NeedsOcrSetup and FileExists(ExpandConstant('{app}\ocr-unavailable.flag')) then
      WizardForm.FinishedLabel.Caption := 'SnipLeaf 已安装，截图、标注和贴图可使用。首次启动将自动检查中英文 OCR，并在独立面板准备缺少的语言；下载需要联网和管理员确认，可选择稍后。'
    else if NeedsOcrSetup then
      WizardForm.FinishedLabel.Caption := 'SnipLeaf 已安装，已有语言可以识别文字。首次启动将自动检查并准备缺少的中英文 OCR 组件，可选择稍后；过程不影响截图和贴图。';
  end;
end;

function ExistingInstallation: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#SetupAppId}}_is1');
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
    '可在安装时启用，也可稍后从设置中修复',
    '勾选后会为当前用户注册 SnipLeaf 的 Windows OCR 本地身份，并信任 SnipLeaf Preview 专用公共证书，不添加根证书；必要时 Windows 会请求管理员确认。未勾选或身份准备失败时，基础安装仍会完成。卸载清理本安装添加的信任。',
    False, False);
  ConsentPage.Add('启用离线 OCR：我同意信任此安装包的 SnipLeaf Preview 专用公共证书');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if ExistingInstallation then
    Result := '请先卸载已安装的 SnipLeaf 试用版，个人设置会保留。';
end;

procedure RegisterApplication;
var
  ResultCode: Integer;
  Parameters: String;
  Started: Boolean;
  CheckCode: Integer;
begin
  Parameters := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    AddQuotes(ExpandConstant('{app}\Installer\Deploy-App.ps1')) + ' -Action Install -Directory ' +
    AddQuotes(ExpandConstant('{app}'));
  if TrustConsent then Parameters := Parameters + ' -TrustPreview';
  WizardForm.StatusLabel.Caption := '正在检查基础功能并准备可选的 OCR 身份…';
  Started := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if Started and (ResultCode = 10) then begin
    RegistrationFailed := True;
    Log('SnipLeaf payload or ownership verification failed. Canceling installation before commit.');
    if not WizardSilent then
      MsgBox('安装载荷或安装记录验证失败，安装将回滚。请查看 %TEMP%\SnipLeaf-install-last.log。', mbError, MB_OK);
    { Inno ignores exceptions in AfterInstall. Request its own cancellation
      while the transaction is still open, including /VERYSILENT mode. }
    WizardForm.Show;
    WizardForm.Close;
  end else if not Started or (ResultCode <> 0) then begin
    Log('Deployment helper did not complete. Retaining basic application files.');
    SaveStringToFile(ExpandConstant('{app}\deployment-helper-needed.flag'), 'Deployment helper unavailable', False);
    SaveStringToFile(ExpandConstant('{app}\ocr-identity-needed.flag'), 'Identity preparation deferred', False);
    Parameters := '--install-check ' + AddQuotes(ExpandConstant('{app}\installation-check.json'));
    if not Exec(ExpandConstant('{app}\SnipLeaf.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, CheckCode) or
      ((CheckCode <> 0) and (CheckCode <> 3)) then
      SaveStringToFile(ExpandConstant('{app}\application-check-needed.flag'), 'Native application check unavailable', False);
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
  if CheckForMutexes('{#SetupMutex}') then begin
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
  if not FileExists(ExpandConstant('{app}\installation-state.json')) then begin
    Log('No completed deployment ownership record; remove basic files without touching any package or certificate.');
    Exit;
  end;
  Parameters := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    AddQuotes(ExpandConstant('{app}\Installer\Deploy-App.ps1')) + ' -Action Uninstall -Directory ' +
    AddQuotes(ExpandConstant('{app}'));
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then begin
    if not UninstallSilent then
      MsgBox('应用身份或测试证书清理未完成，卸载已停止。请查看 %TEMP%\SnipLeaf-install-last.log 后重试。', mbError, MB_OK);
    Abort;
  end;
end;
