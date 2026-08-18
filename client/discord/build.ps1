param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$ClientRoot = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $ClientRoot "..\..")).Path
$BuildRoot = Join-Path $ClientRoot "build"
$CargoTargetDir = Join-Path $BuildRoot "cargo"
$CMakeBuildDir = Join-Path $BuildRoot "cmake-x64"
$StageDir = Join-Path $BuildRoot "package"
$ArtifactsDir = Join-Path $BuildRoot $Config
$ZipPath = Join-Path $BuildRoot "discord-tunnel-windows-x64.zip"
$RuntimeManifest = Join-Path $ClientRoot "runtime\Cargo.toml"
$RustTarget = "x86_64-pc-windows-msvc"
$CargoProfile = if ($Config -eq "Release") { "release" } else { "debug" }

$CargoArgs = @(
    "build",
    "--manifest-path", $RuntimeManifest,
    "--locked",
    "--target", $RustTarget,
    "--target-dir", $CargoTargetDir
)
if ($Config -eq "Release") {
    $CargoArgs += "--release"
}

& cargo @CargoArgs

$RustLib = Join-Path $CargoTargetDir "$RustTarget\$CargoProfile\discord_runtime.lib"
if (-not (Test-Path -LiteralPath $RustLib -PathType Leaf)) {
    throw "Rust static library was not produced: $RustLib"
}

& cmake -S $ClientRoot -B $CMakeBuildDir -A x64 "-DDISCORD_RUNTIME_LIBRARY=$RustLib"
& cmake --build $CMakeBuildDir --config $Config --parallel

$CMakeOutputDir = Join-Path $CMakeBuildDir $Config
$ExePath = Join-Path $CMakeOutputDir "discord-tunnel.exe"
$DllPath = Join-Path $CMakeOutputDir "version.dll"
$LicensePath = Join-Path $RepoRoot "LICENSE"

foreach ($RequiredFile in @($ExePath, $DllPath, $LicensePath)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required artifact is missing: $RequiredFile"
    }
}

Remove-Item -LiteralPath $ArtifactsDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $StageDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Path $ArtifactsDir | Out-Null
New-Item -ItemType Directory -Path $StageDir | Out-Null

Copy-Item -LiteralPath $ExePath -Destination (Join-Path $ArtifactsDir "discord-tunnel.exe")
Copy-Item -LiteralPath $DllPath -Destination (Join-Path $ArtifactsDir "version.dll")

@"
[tunnel]
enabled=1
server=
port=443
token=
ca_cert=
insecure=0
listen_port=17821

[client]
discord_root=
"@ | Set-Content -LiteralPath (Join-Path $ArtifactsDir "discord-tunnel.ini") -Encoding utf8NoBOM

Copy-Item -LiteralPath (Join-Path $ArtifactsDir "discord-tunnel.exe") -Destination (Join-Path $StageDir "discord-tunnel.exe")
Copy-Item -LiteralPath (Join-Path $ArtifactsDir "version.dll") -Destination (Join-Path $StageDir "version.dll")
Copy-Item -LiteralPath (Join-Path $ArtifactsDir "discord-tunnel.ini") -Destination (Join-Path $StageDir "discord-tunnel.ini")
Copy-Item -LiteralPath $LicensePath -Destination (Join-Path $StageDir "LICENSE")

Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host "Artifacts: $ArtifactsDir"
Write-Host "Archive: $ZipPath"
