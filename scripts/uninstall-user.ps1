$ErrorActionPreference = "Stop"

$Destination = Join-Path $env:APPDATA "obs-studio\plugins\obs-voice-isolator"

Get-Process obs64 -ErrorAction SilentlyContinue | ForEach-Object {
    throw "Cierra OBS Studio antes de desinstalar el plugin."
}

if (Test-Path $Destination) {
    Remove-Item -Recurse -Force $Destination
    Write-Host "Plugin eliminado: $Destination"
} else {
    Write-Host "El plugin no estaba instalado en $Destination"
}
