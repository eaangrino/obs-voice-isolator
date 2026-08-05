$ErrorActionPreference = "Stop"

function Write-Check {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Detail
    )

    $status = if ($Ok) { "[OK]" } else { "[FALTA]" }
    Write-Host ("{0,-8} {1,-28} {2}" -f $status, $Name, $Detail)
}

Write-Host "Diagnóstico de herramientas para OBS Voice Isolator"
Write-Host "--------------------------------------------------"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $cmakeVersionText = (& cmake --version | Select-Object -First 1)
    $cmakeVersion = [version](($cmakeVersionText -split " ")[2])
    Write-Check "CMake >= 3.28" ($cmakeVersion -ge [version]"3.28.0") $cmakeVersionText
} else {
    Write-Check "CMake >= 3.28" $false "No está en PATH"
}

$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    Write-Check "Git" $true (& git --version)
} else {
    Write-Check "Git" $false "No está en PATH"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

    Write-Check "Visual Studio C++" ([bool]$vsPath) `
        $(if ($vsPath) { $vsPath } else { "Falta Desktop development with C++" })

    $sdkComponent = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.Windows11SDK.22621 `
        -property installationPath

    if (-not $sdkComponent) {
        $sdkComponent = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.Windows10SDK.20348 `
            -property installationPath
    }

    Write-Check "Windows SDK" ([bool]$sdkComponent) `
        $(if ($sdkComponent) { "SDK compatible detectado" } else { "Instala SDK 10.0.20348 o superior" })
} else {
    Write-Check "Visual Studio Installer" $false "vswhere.exe no encontrado"
}

$obsPaths = @(
    "$env:ProgramFiles\obs-studio\bin\64bit\obs64.exe",
    "$env:ProgramFiles(x86)\obs-studio\bin\64bit\obs64.exe"
)
$obs = $obsPaths | Where-Object { Test-Path $_ } | Select-Object -First 1
Write-Check "OBS Studio x64" ([bool]$obs) `
    $(if ($obs) { $obs } else { "No detectado en la ruta estándar" })

Write-Host ""
Write-Host "Instalación rápida con winget (PowerShell como administrador):"
Write-Host "  winget install Kitware.CMake"
Write-Host "  winget install Git.Git"
Write-Host "  winget install Microsoft.VisualStudio.2022.Community"
Write-Host ""
Write-Host "En Visual Studio Installer agrega:"
Write-Host "  - Desktop development with C++"
Write-Host "  - MSVC v143"
Write-Host "  - Windows 11 SDK 10.0.22621 o superior"
