[CmdletBinding()]
param([ValidateRange(10,600)][int]$Seconds=60)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskExe=(Resolve-Path -LiteralPath (Join-Path $taskProjectRoot 'build\Release\SnapLite.exe')).Path
$taskExisting=Get-Process -Name SnapLite -ErrorAction SilentlyContinue
if($taskExisting){throw 'Exit SnapLite before measuring an idle baseline.'}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SnapLiteIdleProbe {
    private delegate bool Callback(IntPtr hwnd, IntPtr context);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr context);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
    public static int CanvasCount(int processId) {
        int count=0;
        EnumWindows((hwnd,context) => {
            uint pid;
            GetWindowThreadProcessId(hwnd,out pid);
            if(pid==(uint)processId) {
                var name=new StringBuilder(128);
                GetClassName(hwnd,name,name.Capacity);
                if(name.ToString()=="SnapLite.Canvas") count++;
            }
            return true;
        },IntPtr.Zero);
        return count;
    }
}
'@
$taskProcess=Start-Process -FilePath $taskExe -WindowStyle Hidden -PassThru
try{
    Start-Sleep -Seconds 3
    $taskProcess.Refresh()
    if($taskProcess.HasExited){throw 'SnapLite exited before sampling.'}
    $taskCpuBefore=$taskProcess.TotalProcessorTime.TotalMilliseconds
    $taskStart=Get-Date
    $taskSamples=@()
    for($taskIndex=0;$taskIndex -lt $Seconds;$taskIndex+=2){
        Start-Sleep -Seconds 2
        $taskProcess.Refresh()
        if($taskProcess.HasExited){throw 'SnapLite exited during sampling.'}
        $taskSamples+=@{
            elapsedSeconds=((Get-Date)-$taskStart).TotalSeconds
            privateBytes=$taskProcess.PrivateMemorySize64;workingSetBytes=$taskProcess.WorkingSet64
            handles=$taskProcess.HandleCount;threads=$taskProcess.Threads.Count
            canvases=[SnapLiteIdleProbe]::CanvasCount($taskProcess.Id)
            gdi=[SnapLiteIdleProbe]::GetGuiResources($taskProcess.Handle,0)
            user=[SnapLiteIdleProbe]::GetGuiResources($taskProcess.Handle,1)
        }
    }
    $taskDuration=((Get-Date)-$taskStart).TotalMilliseconds
    $taskCpuMs=$taskProcess.TotalProcessorTime.TotalMilliseconds-$taskCpuBefore
    $taskCores=[Environment]::ProcessorCount
    $taskCanvasPeak=($taskSamples.canvases|Measure-Object -Maximum).Maximum
    @{
        state=$(if($taskCanvasPeak -eq 0){'tray idle, no canvas observed'}else{'canvas activity observed; not an idle baseline'})
        canvasPeak=$taskCanvasPeak;durationSeconds=$taskDuration/1000
        samples=$taskSamples.Count;logicalProcessors=$taskCores
        privatePeakMiB=($taskSamples.privateBytes|Measure-Object -Maximum).Maximum/1MB
        privateMeanMiB=($taskSamples.privateBytes|Measure-Object -Average).Average/1MB
        workingSetPeakMiB=($taskSamples.workingSetBytes|Measure-Object -Maximum).Maximum/1MB
        cpuOneCorePercent=$taskCpuMs/$taskDuration*100
        cpuMachinePercent=$taskCpuMs/$taskDuration*100/$taskCores
        handlesFirst=$taskSamples[0].handles;handlesLast=$taskSamples[-1].handles
        applicationBytes=(Get-Item -LiteralPath $taskExe).Length
        measuredAt=(Get-Date).ToString('o')
        measurements=$taskSamples
    }|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $taskProjectRoot 'reports\idle.local.json') -Encoding utf8
    Get-Content -LiteralPath (Join-Path $taskProjectRoot 'reports\idle.local.json') -Raw
}finally{
    if(-not $taskProcess.HasExited){
        $taskQuit=Start-Process -FilePath $taskExe -ArgumentList '--quit' -WindowStyle Hidden -PassThru -Wait
        $taskProcess.WaitForExit(5000)|Out-Null
    }
    $taskProcess.Dispose()
}
