param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [string]$MakeExe = "",

    [switch]$Clean,
    # Flash the built .bin to a USB-connected ClearCore after a successful build.
    [switch]$Flash
)

$ErrorActionPreference = "Stop"

# Command-line build of the Atmel Studio generated Makefiles (same output as building
# Firmware/ClearCoreFirmware/ClearCNC_Firmware.atsln in Microchip/Atmel Studio).
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Projects = @(
    (Join-Path $RepoRoot "LwIP"),
    (Join-Path $RepoRoot "libClearCore"),
    (Join-Path $PSScriptRoot "ClearCoreFirmware")
)

function Find-Make {
    param([string]$UserPath)
    if (-not [string]::IsNullOrWhiteSpace($UserPath)) {
        if (-not (Test-Path -LiteralPath $UserPath)) { throw "make was not found: $UserPath" }
        return (Resolve-Path -LiteralPath $UserPath).Path
    }
    $atmel = Join-Path ${env:ProgramFiles(x86)} "Atmel\Studio\7.0\shellutils\make.exe"
    if (Test-Path -LiteralPath $atmel) {
        return $atmel
    }
    $fromPath = Get-Command make -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }
    throw "make.exe was not found. Install Microchip/Atmel Studio 7 or pass -MakeExe."
}

# Atmel Studio creates the object subfolders itself; plain make needs them to exist.
function Initialize-ObjectDirs {
    param([string]$ConfigDir)
    $inSubdirs = $false
    foreach ($line in Get-Content -LiteralPath (Join-Path $ConfigDir "Makefile")) {
        if ($line -match '^SUBDIRS\s*:=') { $inSubdirs = $true; continue }
        if (-not $inSubdirs) { continue }
        $entry = $line.Trim().TrimEnd('\').Trim()
        if ([string]::IsNullOrEmpty($entry)) { break }
        $rel = $entry -replace '^(\.\./)+', ''
        if ($rel -and $rel -ne '.') {
            New-Item -ItemType Directory -Force -Path (Join-Path $ConfigDir $rel) | Out-Null
        }
    }
}

$MakeExe = Find-Make -UserPath $MakeExe

foreach ($project in $Projects) {
    $configDir = Join-Path $project $Config
    Write-Host "== $(Split-Path -Leaf $project) ($Config)"
    Push-Location -LiteralPath $configDir
    try {
        if ($Clean) {
            & $MakeExe clean
        }
        Initialize-ObjectDirs -ConfigDir $configDir
        & $MakeExe all
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed in $configDir (exit $LASTEXITCODE)."
        }
    } finally {
        Pop-Location
    }
}

$fwDir = Join-Path $PSScriptRoot "ClearCoreFirmware\$Config"
$bin = Join-Path $fwDir "ClearCNC_Firmware.bin"
$uf2 = Join-Path $fwDir "ClearCNC_Firmware.uf2"
& (Join-Path $RepoRoot "tools\uf2-builder\Release\uf2-builder.exe") $bin $uf2
if ($LASTEXITCODE -ne 0) {
    throw "uf2-builder failed with exit code $LASTEXITCODE."
}
Write-Host "Firmware: $bin"
Write-Host "          $uf2"

if ($Flash) {
    & cmd.exe /d /c "`"$(Join-Path $RepoRoot 'tools\flash_clearcore.cmd')`" `"$bin`""
    if ($LASTEXITCODE -ne 0) {
        throw "flash_clearcore.cmd failed with exit code $LASTEXITCODE."
    }
}
