$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $ProjectRoot "dist\obs-voice-isolator"
$PackageDir = Join-Path $ProjectRoot "packages"
$Package = Join-Path $PackageDir "obs-voice-isolator-windows-x64.zip"

if (-not (Test-Path $Dist)) {
    throw "No existe el build instalado en dist. Ejecuta build-windows.ps1."
}

New-Item -ItemType Directory -Force $PackageDir | Out-Null
Remove-Item -Force $Package -ErrorAction SilentlyContinue
Compress-Archive -Path "$Dist\*" -DestinationPath $Package

Write-Host "Paquete creado:"
Write-Host "  $Package"
