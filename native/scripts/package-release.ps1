# SplitFrame Windows release packaging script
# Creates a standalone zip distribution for Windows x64.
# Requires PowerShell 5.1+. Do not use PowerShell 7 syntax (no &&, ??, or ternaries).

[CmdletBinding()]
param(
  [string]$BuildDir = "build\release",
  [string]$OutDir = "dist",
  [Parameter(Mandatory = $true)]
  [string]$Version
)

$ErrorActionPreference = "Stop"

# Determine script and directory locations
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) {
  $scriptDir = (Get-Location).Path
}

# Native root is parent of scripts directory if located inside native/scripts
$nativeDir = $scriptDir
if ((Split-Path -Leaf $nativeDir) -eq "scripts") {
  $nativeDir = Split-Path -Parent $nativeDir
}
$repoRoot = Split-Path -Parent $nativeDir

# Resolve BuildDir
$resolvedBuildDir = $BuildDir
if (-not [System.IO.Path]::IsPathRooted($resolvedBuildDir)) {
  if (Test-Path $resolvedBuildDir) {
    $resolvedBuildDir = (Resolve-Path $resolvedBuildDir).Path
  } elseif (Test-Path (Join-Path $nativeDir $resolvedBuildDir)) {
    $resolvedBuildDir = (Resolve-Path (Join-Path $nativeDir $resolvedBuildDir)).Path
  } else {
    $resolvedBuildDir = Join-Path $nativeDir $BuildDir
  }
}

$binDir = Join-Path $resolvedBuildDir "bin"
$splitframeExe = Join-Path $binDir "splitframe.exe"
$splitframeCliExe = Join-Path $binDir "splitframe-cli.exe"

# Step 1: Check that splitframe.exe and splitframe-cli.exe exist
if ((-not (Test-Path $splitframeExe)) -or (-not (Test-Path $splitframeCliExe))) {
  $presetsPath = Join-Path $nativeDir "CMakePresets.json"
  $presetToUse = "release"
  if (Test-Path $presetsPath) {
    $presetsContent = Get-Content -Raw $presetsPath | ConvertFrom-Json
    $hasRelease = $false
    if ($presetsContent.configurePresets) {
      foreach ($cp in $presetsContent.configurePresets) {
        if ($cp.name -eq "release") {
          $hasRelease = $true
          break
        }
      }
    }
    if (-not $hasRelease) {
      Write-Warning "Preset 'release' not found in CMakePresets.json, falling back to 'debug'"
      $presetToUse = "debug"
    }
  }

  Write-Host "Error: Executables not found in '$binDir'." -ForegroundColor Red
  Write-Host "Build them first using:"
  Write-Host "  .\scripts\dev.cmd cmake --preset $presetToUse"
  Write-Host "  .\scripts\dev.cmd cmake --build --preset $presetToUse -j 2"
  exit 1
}

# Step 2: Create <OutDir>\SplitFrame-<Version>-win64\
$resolvedOutDir = $OutDir
if (-not [System.IO.Path]::IsPathRooted($resolvedOutDir)) {
  $resolvedOutDir = Join-Path (Get-Location).Path $OutDir
}

$destFolderName = "SplitFrame-$Version-win64"
$destDir = Join-Path $resolvedOutDir $destFolderName

if (Test-Path $destDir) {
  Remove-Item -Recurse -Force $destDir
}
New-Item -ItemType Directory -Force -Path $destDir | Out-Null

# Step 3: Copy required files into destination
Write-Host "Packaging SplitFrame $Version into $destDir..."

# 3a. Copy the two exes
Copy-Item $splitframeExe $destDir -Force
Copy-Item $splitframeCliExe $destDir -Force

# 3b. Copy every .dll in <BuildDir>\bin
Get-ChildItem -Path $binDir -Filter "*.dll" | Copy-Item -Destination $destDir -Force

# 3c. Run windeployqt
$windeployqt = "C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe"
$targetExe = Join-Path $destDir "splitframe.exe"

# Locate QML directory
$qmlDir = Join-Path $nativeDir "src\app\qml"
if (-not (Test-Path $qmlDir)) {
  $candidateQml = "..\src\app\qml"
  if (Test-Path $candidateQml) {
    $qmlDir = $candidateQml
  } elseif (Test-Path (Join-Path $nativeDir "src\app")) {
    $qmlDir = Join-Path $nativeDir "src\app"
  } elseif (Test-Path "..\src\app") {
    $qmlDir = "..\src\app"
  } else {
    $qmlDir = $null
  }
}

if (-not (Test-Path $windeployqt)) {
  Write-Error "windeployqt.exe not found at $windeployqt"
  exit 1
}

if ($qmlDir -and (Test-Path $qmlDir)) {
  Write-Host "Running windeployqt with --qmldir $qmlDir..."
  & $windeployqt --release --qmldir $qmlDir $targetExe
} else {
  Write-Host "Running windeployqt..."
  & $windeployqt --release $targetExe
}
if ($LASTEXITCODE -ne 0) {
  Write-Error "windeployqt failed with exit code $LASTEXITCODE"
  exit 1
}

# 3d. Copy FFmpeg DLLs
$ffmpegBin = "C:\dev\ffmpeg-8.1-gpl\bin"
if (-not (Test-Path $ffmpegBin)) {
  Write-Error "FFmpeg directory not found at $ffmpegBin"
  exit 1
}
$ffmpegDlls = Get-ChildItem -Path $ffmpegBin -Filter "*.dll"
if ($ffmpegDlls.Count -eq 0) {
  Write-Error "No FFmpeg DLLs found in $ffmpegBin"
  exit 1
}
$ffmpegDlls | Copy-Item -Destination $destDir -Force

# 3e. Copy OCIO DLLs
$ocioBin = "C:\dev\ocio\bin"
if (-not (Test-Path $ocioBin)) {
  Write-Error "OpenColorIO directory not found at $ocioBin"
  exit 1
}
$ocioDlls = Get-ChildItem -Path $ocioBin -Filter "*.dll"
if ($ocioDlls.Count -eq 0) {
  Write-Error "No OCIO DLLs found in $ocioBin"
  exit 1
}
$ocioDlls | Copy-Item -Destination $destDir -Force

# 3f. Copy onnxruntime.dll if present in build bin
$onnxDll = Join-Path $binDir "onnxruntime.dll"
if (Test-Path $onnxDll) {
  Copy-Item $onnxDll $destDir -Force
}

# 3g. Copy LICENSE, native\README.md, and native\src\engine\comfy_templates\
$licenseCandidates = @(
  (Join-Path $repoRoot "LICENSE"),
  (Join-Path $nativeDir "LICENSE"),
  "LICENSE"
)
foreach ($lic in $licenseCandidates) {
  if (Test-Path $lic) {
    Copy-Item $lic $destDir -Force
    break
  }
}

$readmeCandidates = @(
  (Join-Path $nativeDir "README.md"),
  "native\README.md",
  "README.md"
)
foreach ($rm in $readmeCandidates) {
  if (Test-Path $rm) {
    Copy-Item $rm $destDir -Force
    break
  }
}

$templatesDir = Join-Path $nativeDir "src\engine\comfy_templates"
if (-not (Test-Path $templatesDir)) {
  Write-Error "comfy_templates directory not found at $templatesDir"
  exit 1
}
Copy-Item $templatesDir (Join-Path $destDir "comfy_templates") -Recurse -Force

# Step 4: Write <dest>\THIRD_PARTY.txt
$thirdPartyText = @"
SplitFrame Third-Party Software Notices and Licenses
=====================================================

This release package includes or links against the following third-party components:

1. FFmpeg
   - License: GNU General Public License (GPL) version 3.0 or later (GPL-3.0-or-later)
   - Upstream: https://ffmpeg.org
   - Source code availability: https://ffmpeg.org

2. Qt 6 (6.8.3)
   - License: GNU Lesser General Public License (LGPL) version 3.0
   - Upstream: https://www.qt.io
   - Dynamically linked shared libraries and QML runtime plugins.

3. OpenColorIO (OCIO 2.x)
   - License: BSD 3-Clause License
   - Upstream: https://opencolorio.org

4. ONNX Runtime
   - License: MIT License
   - Upstream: https://onnxruntime.ai

5. gl-transitions
   - License: MIT / BSD-2-Clause / BSD-3-Clause
   - Upstream: https://gl-transitions.com
   - See THIRD_PARTY_GL_TRANSITIONS.md for individual author attributions and shader licenses.

6. MediaPipe Hand Models
   - License: Apache License 2.0
   - Upstream: https://developers.google.com/mediapipe
   - NOTE: Hand landmark model weights are NOT bundled in this distribution and must be downloaded separately if enabled.
"@

Set-Content -Path (Join-Path $destDir "THIRD_PARTY.txt") -Value $thirdPartyText -Encoding UTF8

# Step 5: Zip it to <OutDir>\SplitFrame-<Version>-win64.zip
$zipPath = Join-Path $resolvedOutDir "$destFolderName.zip"
if (Test-Path $zipPath) {
  Remove-Item $zipPath -Force
}

Write-Host "Compressing archive to $zipPath..."
Compress-Archive -Path "$destDir\*" -DestinationPath $zipPath -Force

# Step 6: Print the zip path and its size in MB
if (Test-Path $zipPath) {
  $zipItem = Get-Item $zipPath
  $sizeMB = [math]::Round($zipItem.Length / 1MB, 2)
  Write-Host "Release package ready: $($zipItem.FullName) ($sizeMB MB)" -ForegroundColor Green
} else {
  Write-Error "Failed to produce zip file at $zipPath"
}
