[CmdletBinding()]
param([string]$ReportDirectory,[switch]$Interactive,[ValidatePattern('^[a-zA-Z0-9-]{1,80}$')][string]$RunId)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSEdition -eq 'Desktop'){$env:PSModulePath=Join-Path $PSHOME 'Modules'}

function Write-OcrPreparationReport($Report,[string]$Path) {
    $Report.updatedAt=Get-Date -Format o
    $taskTemporary=$Path+'.tmp'
    [IO.File]::WriteAllText($taskTemporary,($Report|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($true))
    if([IO.File]::Exists($Path)){[IO.File]::Replace($taskTemporary,$Path,[NullString]::Value)}
    else{[IO.File]::Move($taskTemporary,$Path)}
}

function Invoke-OcrLanguagePreparation([string]$ReportPath,[string]$DismLogPath) {
    $taskProgress=[ordered]@{
        schema=1;phase='checking';success=$false;restartNeeded=$false;
        startedAt=(Get-Date -Format o);updatedAt='';activeCapability='';
        capabilities=@();errors=@();dismLogPath=$DismLogPath
    }
    # Keep all four rows present even if a dependency query fails. Unknown or
    # pending components must never be counted as installed by the UI.
    foreach($taskLanguage in @('zh-CN','en-US')){
        foreach($taskFeature in @('Basic','OCR')){
            $taskProgress.capabilities+=@([ordered]@{name=('Language.'+$taskFeature+'~~~'+$taskLanguage+'~0.0.1.0');state='Checking';operation='query'})
        }
    }
    Write-OcrPreparationReport $taskProgress $ReportPath
    # Inspect every target first, so an English language already installed is
    # visible even while a missing Chinese component takes a long time.
    foreach($taskRecord in $taskProgress.capabilities){
        $taskProgress.activeCapability=$taskRecord.name
        Write-OcrPreparationReport $taskProgress $ReportPath
        try{
            $taskRecord.state=[string](Get-WindowsCapability -Online -Name $taskRecord.name -ErrorAction Stop).State
            $taskRecord.operation=if($taskRecord.state -eq 'Installed'){'complete'}else{'pending'}
            if($taskRecord.state -eq 'InstallPending'){$taskProgress.restartNeeded=$true}
        }catch{
            $taskRecord.state='QueryFailed';$taskRecord.operation='failed'
            $taskProgress.errors+=@([ordered]@{language=($taskRecord.name -split '~~~|~')[1];capability=$taskRecord.name;message=$_.Exception.Message;hresult=('0x{0:X8}' -f $_.Exception.HResult)})
        }
        Write-OcrPreparationReport $taskProgress $ReportPath
    }
    foreach($taskLanguage in @('zh-CN','en-US')){
        $taskRecord=$null
        try{
            # Microsoft documents Language.Basic as a prerequisite of Language.OCR.
            # Speech, handwriting and other unrelated language features are omitted.
            foreach($taskFeature in @('Basic','OCR')){
                $taskName='Language.'+$taskFeature+'~~~'+$taskLanguage+'~0.0.1.0'
                $taskRecord=@($taskProgress.capabilities|Where-Object {$_.name -eq $taskName})[0]
                $taskProgress.activeCapability=$taskName
                if($taskRecord.state -eq 'QueryFailed'){break}
                if($taskRecord.state -eq 'InstallPending'){
                    $taskProgress.restartNeeded=$true
                    throw ('Windows 要求先重启，再继续准备语言组件：'+$taskName)
                }
                if($taskRecord.state -ne 'Installed'){
                    $taskRecord.operation='install'
                    $taskProgress.phase='installing'
                    Write-OcrPreparationReport $taskProgress $ReportPath
                    Write-Host ('正在下载或安装 '+$taskName+'，进度由 Windows 提供。') -ForegroundColor Cyan
                    Write-Host '此步骤可能较慢；不会影响已安装的 SnipLeaf，也不会触发软件回滚。'
                    $taskAdded=Add-WindowsCapability -Online -Name $taskName -LogPath $DismLogPath -ErrorAction Stop
                    $taskProgress.restartNeeded=$taskProgress.restartNeeded -or [bool]$taskAdded.RestartNeeded
                    $taskRecord.state=[string](Get-WindowsCapability -Online -Name $taskName -ErrorAction Stop).State
                    if($taskRecord.state -ne 'Installed'){
                        if($taskRecord.state -eq 'InstallPending'){$taskProgress.restartNeeded=$true}
                        throw ('Windows 返回后组件仍未就绪：'+$taskName+' ('+$taskRecord.state+')')
                    }
                }
                $taskRecord.operation='complete'
                Write-OcrPreparationReport $taskProgress $ReportPath
                Write-Host ('已就绪：'+$taskName) -ForegroundColor Green
            }
        }catch{
            if($taskRecord){
                $taskRecord.operation='failed'
            }
            $taskProgress.errors+=@([ordered]@{language=$taskLanguage;capability=$taskProgress.activeCapability;message=$_.Exception.Message;hresult=('0x{0:X8}' -f $_.Exception.HResult)})
            Write-OcrPreparationReport $taskProgress $ReportPath
            Write-Host $_.Exception.Message -ForegroundColor Red
            # A failure of one language must not discard the other language.
        }
    }
    $taskProgress.activeCapability=''
    $taskProgress.success=$taskProgress.errors.Count -eq 0
    $taskProgress.phase=if($taskProgress.success){'completed'}elseif($taskProgress.restartNeeded){'restart-required'}else{'failed'}
    Write-OcrPreparationReport $taskProgress $ReportPath
    return [pscustomobject]$taskProgress
}

$taskExitCode=1
$taskMutex=$null
$taskLocked=$false
$taskRunDirectory=$null
try{
    Write-Host 'SnipLeaf · 独立 OCR 语言准备' -ForegroundColor Cyan
    Write-Host '只准备 Windows 中文、英文的基础语言和 OCR 组件。初次下载需要联网，以后识别可离线。'
    if(-not $ReportDirectory){$ReportDirectory=Join-Path $env:LOCALAPPDATA 'SnipLeaf\Diagnostics'}
    if(-not $RunId){$RunId=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)}
    $taskRunDirectory=Join-Path ([IO.Path]::GetFullPath($ReportDirectory)) $RunId
    [IO.Directory]::CreateDirectory($taskRunDirectory)|Out-Null
    $taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
        throw '此操作需要管理员权限，请从 SnipLeaf 设置中的“配置 OCR 语言”重新打开并响应 Windows 确认。'
    }
    $taskMutex=[Threading.Mutex]::new($false,'Global\SnipLeaf.OcrLanguageSetup')
    try{$taskLocked=$taskMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$taskLocked=$true}
    if(-not $taskLocked){throw '另一项 SnipLeaf OCR 语言准备仍在运行，请等待其结束；无需重复安装 SnipLeaf。'}
    Write-Host ('本次进度和日志：'+$taskRunDirectory)
    $taskResult=Invoke-OcrLanguagePreparation (Join-Path $taskRunDirectory 'ocr-prerequisites.json') (Join-Path $taskRunDirectory 'dism.log')
    if($taskResult.success){
        $taskExitCode=0
        Write-Host '中英文 OCR 组件已就绪。请退出再启动 SnipLeaf，以刷新识别语言。' -ForegroundColor Green
    }else{
        Write-Host '部分组件未能准备完成；SnipLeaf 保持安装，已有语言仍可使用。请保留本窗口显示的日志。' -ForegroundColor Yellow
        Write-Host '可在 Windows 设置 → 时间和语言 → 语言和区域中查看语言功能；下载失败请检查 Windows 更新。'
    }
    if($taskResult.restartNeeded){Write-Host 'Windows 报告需要重启。请保存工作后按系统提示重启，再检查 OCR。' -ForegroundColor Yellow}
}catch{
    Write-Host $_.Exception.Message -ForegroundColor Red
    if($taskRunDirectory){
        $taskFatal=[ordered]@{schema=1;phase='failed';success=$false;restartNeeded=$false;updatedAt='';activeCapability='';capabilities=@();errors=@([ordered]@{message=$_.Exception.Message})}
        Write-OcrPreparationReport $taskFatal (Join-Path $taskRunDirectory 'ocr-prerequisites.json')
        Write-Host ('日志目录：'+$taskRunDirectory)
    }
}finally{
    if($taskLocked){$taskMutex.ReleaseMutex()}
    if($taskMutex){$taskMutex.Dispose()}
    if($Interactive){[void](Read-Host '按 Enter 关闭此窗口')}
}
exit $taskExitCode
