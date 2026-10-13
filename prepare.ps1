param([switch]$RefreshSeed)
$ErrorActionPreference = 'Stop'
$dgmpRoot = $PSScriptRoot
trap {
    $failure = $_ | Out-String
    $failure += $_.ScriptStackTrace
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'logs\prepare-error.log'), $failure)
    exit 1
}
$gameRoot = Split-Path -Parent $dgmpRoot
$sourceBin = Join-Path $gameRoot 'BendGame\Binaries\Win64'
$runtimeRoot = Join-Path $dgmpRoot 'runtime'
$runtimeBin = Join-Path $runtimeRoot 'BendGame\Binaries\Win64'
$profile = Join-Path $dgmpRoot 'profiles\editor\BendGame\Saved'
if (Get-Process -Name DaysGone -ErrorAction SilentlyContinue | Where-Object { !$_.HasExited }) { throw 'Close Days Gone before preparing or launching DGMP.' }
if (!(Test-Path -LiteralPath (Join-Path $dgmpRoot 'build\version.dll'))) { throw 'Build DGMP first: run build.bat.' }
New-Item -ItemType Directory -Path $runtimeBin,$profile,(Join-Path $dgmpRoot 'logs'),(Join-Path $dgmpRoot 'maps') -Force | Out-Null
foreach ($name in @('telemetry.json','control.ini')) {
    $filePath = Join-Path $dgmpRoot $name
    if (Test-Path -LiteralPath $filePath) { Remove-Item -LiteralPath $filePath -Force }
}
function Ensure-Junction([string]$Path, [string]$Target) {
    if (Test-Path -LiteralPath $Path) {
        $existing = Get-Item -LiteralPath $Path -Force
        if (!($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -or [IO.Path]::GetFullPath([string]$existing.Target) -ne [IO.Path]::GetFullPath($Target)) { throw "Unexpected directory at $Path; refusing to replace it." }
    } else { New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null }
}
Ensure-Junction (Join-Path $runtimeRoot 'Engine') (Join-Path $gameRoot 'Engine')
$runtimeContent = Join-Path $runtimeRoot 'BendGame\Content'
if (Test-Path -LiteralPath $runtimeContent) {
    $contentItem = Get-Item -LiteralPath $runtimeContent -Force
    if ($contentItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        if ([IO.Path]::GetFullPath([string]$contentItem.Target) -ne [IO.Path]::GetFullPath((Join-Path $gameRoot 'BendGame\Content'))) { throw 'Unexpected Content junction target.' }
        # Remove only the junction itself, never recurse into shared game content.
        Remove-Item -LiteralPath $runtimeContent -Force
    }
}
New-Item -ItemType Directory -Path $runtimeContent,(Join-Path $runtimeContent 'Movies') -Force | Out-Null
foreach ($dir in Get-ChildItem -LiteralPath (Join-Path $gameRoot 'BendGame\Content') -Directory) {
    if ($dir.Name -ne 'Movies') { Ensure-Junction (Join-Path $runtimeContent $dir.Name) $dir.FullName }
}
# The executable and its proxy are private; never copy the co-op proxy or its configuration.
foreach ($file in Get-ChildItem -LiteralPath $sourceBin -File) {
    if ($file.Name -eq 'DaysGone.exe' -or $file.Name -eq 'NOTICE' -or ($file.Extension -eq '.dll' -and $file.Name -notin @('version.dll','dwmapi.dll','xinput1_3.dll'))) {
        $dest = Join-Path $runtimeBin $file.Name
        if (!(Test-Path -LiteralPath $dest) -or (Get-Item -LiteralPath $dest).Length -ne $file.Length -or (Get-Item -LiteralPath $dest).LastWriteTimeUtc -ne $file.LastWriteTimeUtc) { Copy-Item -LiteralPath $file.FullName -Destination $dest -Force }
    }
}
Copy-Item -LiteralPath (Join-Path $dgmpRoot 'build\version.dll') -Destination (Join-Path $runtimeBin 'version.dll') -Force
Set-Content -LiteralPath (Join-Path $runtimeBin 'steam_appid.txt') -Value '1259420' -Encoding ASCII
. (Join-Path $dgmpRoot 'world-template.ps1')
$template = Initialize-DGMPWorldProfile $dgmpRoot (Join-Path $dgmpRoot 'worlds\bootstrap') (Get-DGMPAccountId)
if ($RefreshSeed) { Write-Output 'The bundled template has been restored; personal saves are never imported.' }
Write-Output "DGMP prepared. Template: $($template.id); runtime: $runtimeBin; isolated profile: $profile"
