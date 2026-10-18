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
# The zip is made from a clean staging folder. The ready-to-run folder ($dist) is
# only updated in place: a player's saves, settings and stored ROM in its data\
# folder are kept.
$stage = Join-Path $root 'build\dist\stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'data\models') | Out-Null

foreach ($f in 'HeyYouPikachuRecompiled.exe', 'SDL2.dll', 'dxil.dll', 'dxcompiler.dll') {
    Copy-Item (Join-Path $bin $f) $stage
}
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'COPYING') $stage
Copy-Item (Join-Path $root 'lib\whisper.cpp\LICENSE') (Join-Path $stage 'LICENSE-whisper.cpp.txt')

$model = Join-Path $root 'models\ggml-base.en.bin'
if (-not (Test-Path $model)) { throw "speech model missing: download ggml-base.en.bin into models\ (see README)" }
Copy-Item $model (Join-Path $stage 'data\models')

New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $stage '*') $dist -Recurse -Force

$zip = Join-Path $root 'build\dist\HeyYouPikachuRecompiled-win64.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
"packaged: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
