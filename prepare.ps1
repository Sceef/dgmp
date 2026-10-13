param([switch]$RefreshSeed)
$ErrorActionPreference = 'Stop'
$dgmpRoot = $PSScriptRoot
$gameRoot = Split-Path -Parent $dgmpRoot
$sourceBin = Join-Path $gameRoot 'BendGame\Binaries\Win64'
$runtimeRoot = Join-Path $dgmpRoot 'runtime'
$runtimeBin = Join-Path $runtimeRoot 'BendGame\Binaries\Win64'
$profile = Join-Path $dgmpRoot 'profiles\editor\BendGame\Saved'
if (Get-Process -Name DaysGone -ErrorAction SilentlyContinue) { throw 'Close Days Gone before preparing or launching DGMP.' }
if (!(Test-Path -LiteralPath (Join-Path $dgmpRoot 'build\version.dll'))) { throw 'Build DGMP first: run build.bat.' }
New-Item -ItemType Directory -Path $runtimeBin,$profile,(Join-Path $dgmpRoot 'logs'),(Join-Path $dgmpRoot 'maps') -Force | Out-Null
function Ensure-Junction([string]$Path, [string]$Target) {
    if (Test-Path -LiteralPath $Path) {
        $existing = Get-Item -LiteralPath $Path -Force
        if (!($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -or [IO.Path]::GetFullPath([string]$existing.Target) -ne [IO.Path]::GetFullPath($Target)) { throw "Unexpected directory at $Path; refusing to replace it." }
    } else { New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null }
}
Ensure-Junction (Join-Path $runtimeRoot 'Engine') (Join-Path $gameRoot 'Engine')
Ensure-Junction (Join-Path $runtimeRoot 'BendGame\Content') (Join-Path $gameRoot 'BendGame\Content')
# The executable and its proxy are private; never copy the co-op proxy or its configuration.
foreach ($file in Get-ChildItem -LiteralPath $sourceBin -File) {
    if ($file.Name -eq 'DaysGone.exe' -or $file.Name -eq 'NOTICE' -or ($file.Extension -eq '.dll' -and $file.Name -notin @('version.dll','dwmapi.dll','xinput1_3.dll'))) {
        $dest = Join-Path $runtimeBin $file.Name
        if (!(Test-Path -LiteralPath $dest) -or (Get-Item -LiteralPath $dest).Length -ne $file.Length -or (Get-Item -LiteralPath $dest).LastWriteTimeUtc -ne $file.LastWriteTimeUtc) { Copy-Item -LiteralPath $file.FullName -Destination $dest -Force }
    }
}
Copy-Item -LiteralPath (Join-Path $dgmpRoot 'build\version.dll') -Destination (Join-Path $runtimeBin 'version.dll') -Force
Set-Content -LiteralPath (Join-Path $runtimeBin 'steam_appid.txt') -Value '1259420' -Encoding ASCII
$sourceSaved = Join-Path $env:LOCALAPPDATA 'BendGame\Saved'
$manifestPath = Join-Path $dgmpRoot 'profiles\editor\seed.json'
if (!(Test-Path -LiteralPath $manifestPath) -or $RefreshSeed) {
    $accounts = @(Get-ChildItem -LiteralPath $sourceSaved -Directory | Where-Object { $_.Name -match '^\d{17}$' })
    $saves = @(foreach ($account in $accounts) {
        $folder = Join-Path $account.FullName 'SaveGames'
        if (Test-Path -LiteralPath $folder) {
            Get-ChildItem -LiteralPath $folder -Filter '*.sav' -File | Where-Object { $_.Name -match '^(autosave\d+|slot\d+|quicksave\d*)\.sav$' -and $_.Length -gt 200000 }
        }
    })
    $seed = $saves | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (!$seed) { throw 'No gameplay save found. DGMP v0.1 needs a local save as its temporary world seed.' }
    $accountName = Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $seed.FullName))
    $destSaves = Join-Path $profile "$accountName\SaveGames"
    New-Item -ItemType Directory -Path $destSaves -Force | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $seed.DirectoryName -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destSaves $file.Name) -Force }
    Copy-Item -LiteralPath $seed.FullName -Destination (Join-Path $destSaves 'slot01.sav') -Force
    $sourceConfig = Join-Path $sourceSaved 'Config'
    if (Test-Path -LiteralPath $sourceConfig) { Copy-Item -LiteralPath $sourceConfig -Destination $profile -Recurse -Force }
    # Keep an immutable bootstrap slot outside the game's save directory.
    Copy-Item -LiteralPath $seed.FullName -Destination (Join-Path $dgmpRoot 'profiles\editor\world-seed.sav') -Force
    @{ source = $seed.FullName; account = $accountName; sha256 = (Get-FileHash -LiteralPath $seed.FullName).Hash; created = (Get-Date).ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$bootstrapFolder = Join-Path $profile "$($manifest.account)\SaveGames"
Copy-Item -LiteralPath (Join-Path $dgmpRoot 'profiles\editor\world-seed.sav') -Destination (Join-Path $bootstrapFolder 'slot01.sav') -Force
Write-Output "DGMP prepared. Runtime: $runtimeBin; isolated profile: $profile"
