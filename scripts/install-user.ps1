$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuiltPlugin = Join-Path $ProjectRoot "dist\obs-voice-isolator"
$Destination = Join-Path $env:APPDATA "obs-studio\plugins\obs-voice-isolator"

if (-not (Test-Path $BuiltPlugin)) {
    throw "No existe $BuiltPlugin. Ejecuta primero scripts\build-windows.ps1"
}

Get-Process obs64 -ErrorAction SilentlyContinue | ForEach-Object {
    throw "Cierra OBS Studio antes de instalar el plugin."
}

Remove-Item -Recurse -Force $Destination -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Destination | Out-Null
Copy-Item -Recurse -Force "$BuiltPlugin\*" $Destination

Write-Host "Plugin instalado en:"
Write-Host "  $Destination"
Write-Host ""
Write-Host "Abre OBS y agrega el filtro 'Aislador de voz (agresivo)' al micrófono principal."
