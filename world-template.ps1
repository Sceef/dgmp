# Template installation is separate from game startup and never opens personal saves.
function Get-DGMPFileHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Get-DGMPWorldTemplate([string]$TemplateRoot) {
    $manifestFile = Join-Path $TemplateRoot 'manifest.json'
    if (!(Test-Path -LiteralPath $manifestFile -PathType Leaf)) { throw 'The bundled DGMP world template is missing.' }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.gameBuild -ne 1752517958 -or $manifest.id -notmatch '^[a-z0-9-]+$') { throw 'Unsupported DGMP world template manifest.' }
    $files = @($manifest.files)
    if ($files.Count -ne 2 -or @($files | Where-Object name -eq 'world.sav').Count -ne 1 -or @($files | Where-Object name -eq 'profile.sav').Count -ne 1) { throw 'The world template must contain world.sav and profile.sav.' }
    foreach ($entry in $files) {
        if ($entry.name -notin @('world.sav','profile.sav') -or $entry.sha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Invalid world template file entry.' }
        $file = Join-Path $TemplateRoot $entry.name
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne $entry.bytes -or (Get-DGMPFileHash $file) -ne $entry.sha256) { throw "World template integrity check failed: $($entry.name)" }
    }
    return $manifest
}

function Get-DGMPAccountId {
    $steam = Get-ItemProperty 'HKCU:\Software\Valve\Steam\ActiveProcess' -ErrorAction SilentlyContinue
    if (!$steam -or !$steam.ActiveUser) { throw 'Start Steam and sign in before preparing DGMP.' }
    return ([UInt64]76561197960265728 + [UInt64]$steam.ActiveUser).ToString()
}

function Initialize-DGMPWorldProfile([string]$DgmpRoot,[string]$TemplateRoot,[string]$AccountId) {
    if ($AccountId -notmatch '^\d{17}$') { throw 'Invalid Steam account ID.' }
    $manifest = Get-DGMPWorldTemplate $TemplateRoot
    $root = [IO.Path]::GetFullPath($DgmpRoot).TrimEnd('\')
    $editor = Join-Path $root 'profiles\editor'
    $profile = Join-Path $editor 'BendGame\Saved'
    $accountRoot = Join-Path $profile $AccountId
    $saves = Join-Path $accountRoot 'SaveGames'
    $installedPath = Join-Path $editor 'seed.json'
    $world = @($manifest.files | Where-Object name -eq 'world.sav')[0]
    $profileFile = @($manifest.files | Where-Object name -eq 'profile.sav')[0]
    $installed = if (Test-Path -LiteralPath $installedPath) { Get-Content -LiteralPath $installedPath -Raw | ConvertFrom-Json } else { $null }
    $changed = !$installed -or $installed.source -ne 'bundled-template' -or $installed.sha256 -ne $world.sha256 -or $installed.profileSha256 -ne $profileFile.sha256 -or $installed.account -ne $AccountId
    if ($changed) {
        $backup = Join-Path $root ('profiles\backups\before-' + $manifest.id + '-' + (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff'))
        # Every recursive directory move is bounded to this private DGMP folder.
        foreach ($target in @($saves,$backup)) {
            if (![IO.Path]::GetFullPath($target).StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Profile path escaped DGMP.' }
            for ($parent=$target; $parent -and [IO.Path]::GetFullPath($parent).StartsWith($root,[StringComparison]::OrdinalIgnoreCase); $parent=Split-Path -Parent $parent) {
                if (Test-Path -LiteralPath $parent) {
                    $item = Get-Item -LiteralPath $parent -Force
                    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing redirected profile path: $parent" }
                }
                if ([IO.Path]::GetFullPath($parent) -eq $root) { break }
            }
        }
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        foreach ($name in @('seed.json','world-seed.sav')) {
            $previous = Join-Path $editor $name
            if (Test-Path -LiteralPath $previous -PathType Leaf) { Copy-Item -LiteralPath $previous -Destination (Join-Path $backup $name) }
        }
        if (Test-Path -LiteralPath $saves) {
            $directory = Get-Item -LiteralPath $saves -Force
            if ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to move a redirected SaveGames directory.' }
            Move-Item -LiteralPath $saves -Destination (Join-Path $backup 'SaveGames')
        }
    }
    New-Item -ItemType Directory -Path $saves -Force | Out-Null
    # Only these two canonical files are installed. No autosaves/checkpoints,
    # difficulty, inventory or profile data are imported from the local player.
    Copy-Item -LiteralPath (Join-Path $TemplateRoot 'world.sav') -Destination (Join-Path $saves 'slot01.sav') -Force
    Copy-Item -LiteralPath (Join-Path $TemplateRoot 'profile.sav') -Destination (Join-Path $saves 'profile.sav') -Force
    Copy-Item -LiteralPath (Join-Path $TemplateRoot 'world.sav') -Destination (Join-Path $editor 'world-seed.sav') -Force
    @{ schema=1; source='bundled-template'; template=$manifest.id; account=$AccountId; sha256=$world.sha256; profileSha256=$profileFile.sha256; installedUtc=(Get-Date).ToUniversalTime().ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath $installedPath -Encoding UTF8
    return $manifest
}
