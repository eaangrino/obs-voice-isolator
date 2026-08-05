param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$Description,

        [Parameter(Mandatory)]
        [scriptblock]$Command
    )

    Write-Host $Description
    & $Command

    if ($LASTEXITCODE -ne 0) {
        throw "El comando falló con código de salida $LASTEXITCODE."
    }
}

if ($Clean) {
    Remove-Item -Recurse -Force build_x64, dist -ErrorAction SilentlyContinue
}

Invoke-Checked "1/3 Configurando..." {
    cmake --preset windows-x64
}

Invoke-Checked "2/3 Compilando RelWithDebInfo..." {
    cmake --build --preset windows-x64 --parallel
}

Invoke-Checked "3/3 Instalando en dist..." {
    cmake --install build_x64 `
        --config RelWithDebInfo `
        --prefix "$ProjectRoot\dist"
}

$DllPath = Join-Path $ProjectRoot `
    "dist\obs-voice-isolator\bin\64bit\obs-voice-isolator.dll"

if (-not (Test-Path $DllPath)) {
    throw "El build terminó, pero no se encontró la DLL esperada: $DllPath"
}

Write-Host ""
Write-Host "Build completado correctamente."
Write-Host "DLL:"
Write-Host "  $DllPath"
Write-Host ""
Write-Host "Para instalar para tu usuario:"
Write-Host "  powershell -ExecutionPolicy Bypass -File .\scripts\install-user.ps1"
