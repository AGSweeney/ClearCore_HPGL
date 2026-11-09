# MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
#
# Builds the Windows installer: dist\ClearHPGL-<version>-setup.exe (Inno Setup 6).
#   1. the firmware (Firmware\build.ps1 -Config Release)
#   2. the app (..\build.ps1: configure, build, windeployqt with the MSVC runtime, the firmware and bossac
#      copied into firmware\ next to the exe)
#   3. stages the app (no test programs), the command-line flasher and the documents in
#      ProjectData\build\installer_stage
#   4. ISCC installer\ClearHPGL.iss with the version from Firmware\ClearCoreFirmware\PlotterConfig.h
#
#   .\installer\build_installer.ps1              # everything
#   .\installer\build_installer.ps1 -SkipBuild   # stage and package what is built already

param(
    [switch]$SkipBuild,
    [string]$Iscc = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

$config = Get-Content -Raw (Join-Path $Root "Firmware\ClearCoreFirmware\PlotterConfig.h")
if ($config -notmatch '#define CHPGL_FIRMWARE_VERSION "([0-9.]+)"') { throw "CHPGL_FIRMWARE_VERSION not found" }
$Version = $Matches[1]
Write-Host "ClearHPGL $Version"

if (-not $SkipBuild) {
    # Firmware first: the app build copies it next to the exe for the firmware updater.
    & (Join-Path $Root "Firmware\build.ps1") -Config Release
    & (Join-Path $Root "build.ps1")
}

$release = Join-Path $Root "ProjectData\build\Release"
$fwBin = Join-Path $release "firmware\ClearHPGL_Firmware.bin"
foreach ($f in @((Join-Path $release "ClearHPGL.exe"), (Join-Path $release "Qt6Core.dll"), $fwBin,
                 (Join-Path $release "firmware\bossac\bossac.exe"))) {
    if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f (build first, without -SkipBuild)" }
}
# The firmware the installer carries must be the version it is named for.
$image = [System.IO.File]::ReadAllBytes($fwBin)
if ([System.Text.Encoding]::ASCII.GetString($image).IndexOf($Version) -lt 0) {
    throw "The firmware next to the app is not version $Version (rebuild without -SkipBuild)"
}

$stage = Join-Path $Root "ProjectData\build\installer_stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$app = New-Item -ItemType Directory -Force (Join-Path $stage "app")
$flash = New-Item -ItemType Directory -Force (Join-Path $stage "flash")
$docs = New-Item -ItemType Directory -Force (Join-Path $stage "docs")

# The app: the deployed Release folder without the test programs and build leftovers.
# Programs are listed by what ships, so a new test program stays out by itself.
$shipExe = @("ClearHPGL.exe", "vc_redist.x64.exe")
Get-ChildItem -LiteralPath $release -File | Where-Object {
    ($_.Extension -ne ".exe" -or $shipExe -contains $_.Name) -and $_.Extension -notin @(".lib", ".pdb", ".exp", ".ilk")
} | Copy-Item -Destination $app
Get-ChildItem -LiteralPath $release -Directory | Where-Object { $_.Name -notlike "*.dir" -and $_.Name -ne "installer_stage" } |
    Copy-Item -Destination $app -Recurse
Copy-Item (Join-Path $Root "tools\bossac\LICENSE") (Join-Path $app "firmware\bossac\LICENSE.txt")

# The command-line flasher, installed into firmware\ beside the firmware and bossac.
Copy-Item (Join-Path $Root "tools\flash_clearcore.cmd") $flash
Copy-Item (Join-Path $PSScriptRoot "flash_firmware.cmd") $flash

# Documents and licences.
foreach ($d in @("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "Firmware\PROTOCOL.md", "docs\manual\clearhpgl-manual.pdf")) {
    Copy-Item (Join-Path $Root $d) $docs
}

if ([string]::IsNullOrWhiteSpace($Iscc)) {
    $Iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $Iscc) { throw "ISCC.exe (Inno Setup 6) not found; pass -Iscc" }
New-Item -ItemType Directory -Force (Join-Path $Root "dist") | Out-Null
& $Iscc "/DAppVersion=$Version" "/DStageDir=$stage" (Join-Path $PSScriptRoot "ClearHPGL.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
Get-Item (Join-Path $Root "dist\ClearHPGL-$Version-setup.exe") | Format-Table Name, Length -AutoSize
