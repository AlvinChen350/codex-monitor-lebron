param([switch]$Once, [switch]$NoDiscord, [switch]$NoAnchor)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$exe = Join-Path $PSScriptRoot 'build\Release\codex-monitor.exe'
if (-not (Test-Path $exe)) { throw 'Run .\build.ps1 first.' }
if (-not $NoDiscord -and -not $env:DISCORD_WEBHOOK_URL) {
    $secret = Read-Host 'Discord webhook URL' -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secret)
    try {
        $env:DISCORD_WEBHOOK_URL = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
}
$config = Join-Path $PSScriptRoot 'config.ini'
if (Test-Path (Join-Path $PSScriptRoot 'config.local.ini')) {
    $config = Join-Path $PSScriptRoot 'config.local.ini'
}
$monitorArgs = @('--config', $config)
if ($Once) { $monitorArgs += '--once' }
if ($NoDiscord) { $monitorArgs += '--no-discord' }
if ($NoAnchor) { $monitorArgs += '--no-anchor' }
& $exe @monitorArgs
exit $LASTEXITCODE
