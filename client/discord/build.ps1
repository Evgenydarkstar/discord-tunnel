param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ClientRoot = Join-Path $Root "discord-tunnel"
$ClientBuild = Join-Path $ClientRoot "scripts\build.ps1"
$ClientOutput = Join-Path $ClientRoot "build-output"
$Output = Join-Path $Root "build\$Config"

& pwsh -File $ClientBuild -Config $Config
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null
Copy-Item -Force (Join-Path $ClientOutput "discord-tunnel.exe") $Output
Copy-Item -Force (Join-Path $ClientOutput "version.dll") $Output
Copy-Item -Force (Join-Path $ClientOutput "discord-tunnel.ini") $Output

Write-Host "Artifacts: $Output"
