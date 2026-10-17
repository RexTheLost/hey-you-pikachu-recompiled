# Build Release and package a ready-to-run folder + zip (no ROM, no game data).
# Includes the Whisper speech model (models\ggml-base.en.bin, MIT) for the microphone.
# Usage (repo root): powershell -File tools\package.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

cmd /c "tools\build.bat Release"
if ($LASTEXITCODE -ne 0) { throw "Release build failed" }

$bin = Join-Path $root 'build\win-Release'
$dist = Join-Path $root 'build\dist\HeyYouPikachuRecompiled'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist | Out-Null

foreach ($f in 'HeyYouPikachuRecompiled.exe', 'SDL2.dll', 'dxil.dll', 'dxcompiler.dll') {
    Copy-Item (Join-Path $bin $f) $dist
}
Copy-Item (Join-Path $root 'README.md') $dist
Copy-Item (Join-Path $root 'COPYING') $dist
Copy-Item (Join-Path $root 'lib\whisper.cpp\LICENSE') (Join-Path $dist 'LICENSE-whisper.cpp.txt')

$model = Join-Path $root 'models\ggml-base.en.bin'
if (-not (Test-Path $model)) { throw "speech model missing: download ggml-base.en.bin into models\ (see README)" }
New-Item -ItemType Directory -Force (Join-Path $dist 'data\models') | Out-Null
Copy-Item $model (Join-Path $dist 'data\models')

$zip = Join-Path $root 'build\dist\HeyYouPikachuRecompiled-win64.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip
"packaged: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
