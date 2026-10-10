[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$taskResources=Join-Path (Split-Path -Parent $PSScriptRoot) 'resources'
$taskAssets=Join-Path $taskResources 'Assets'
[IO.Directory]::CreateDirectory($taskAssets)|Out-Null
foreach($taskSize in @(44,50,150,256)){
    $taskBitmap=New-Object Drawing.Bitmap($taskSize,$taskSize)
    $taskGraphics=[Drawing.Graphics]::FromImage($taskBitmap)
    $taskGraphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $taskGraphics.Clear([Drawing.Color]::FromArgb(36,38,41))
    $taskScale=$taskSize/64.0
    $taskGraphics.ScaleTransform([float]$taskScale,[float]$taskScale)
    $taskPen=New-Object Drawing.Pen([Drawing.Color]::FromArgb(71,196,168),4.0)
    $taskGraphics.DrawLine($taskPen,15,26,15,15);$taskGraphics.DrawLine($taskPen,15,15,26,15)
    $taskGraphics.DrawLine($taskPen,38,15,49,15);$taskGraphics.DrawLine($taskPen,49,15,49,26)
    $taskGraphics.DrawLine($taskPen,15,38,15,49);$taskGraphics.DrawLine($taskPen,15,49,26,49)
    $taskGraphics.DrawLine($taskPen,49,38,49,49);$taskGraphics.DrawLine($taskPen,38,49,49,49)
    $taskPen.Color=[Drawing.Color]::White;$taskPen.Width=2.8
    $taskGraphics.DrawLine($taskPen,26,25,38,25);$taskGraphics.DrawLine($taskPen,28,25,28,33)
    $taskGraphics.DrawLine($taskPen,36,25,36,33);$taskGraphics.DrawLine($taskPen,28,33,24,37)
    $taskGraphics.DrawLine($taskPen,36,33,40,37);$taskGraphics.DrawLine($taskPen,24,37,40,37)
    $taskGraphics.DrawLine($taskPen,32,37,32,44)
    $taskBitmap.Save((Join-Path $taskAssets ("Logo$taskSize.png")),[Drawing.Imaging.ImageFormat]::Png)
    $taskPen.Dispose();$taskGraphics.Dispose();$taskBitmap.Dispose()
}
$taskPng=[IO.File]::ReadAllBytes((Join-Path $taskAssets 'Logo256.png'))
$taskStream=[IO.File]::Create((Join-Path $taskResources 'SnapLite.ico'))
$taskWriter=New-Object IO.BinaryWriter($taskStream)
$taskWriter.Write([uint16]0);$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]1)
$taskWriter.Write([byte]0);$taskWriter.Write([byte]0);$taskWriter.Write([byte]0);$taskWriter.Write([byte]0)
$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]32);$taskWriter.Write([uint32]$taskPng.Length);$taskWriter.Write([uint32]22)
$taskWriter.Write($taskPng);$taskWriter.Dispose();$taskStream.Dispose()
Write-Output 'Generated original SnapLite icons and package assets.'

