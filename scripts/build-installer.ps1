$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

$BuildSpecPath = Join-Path $ProjectRoot "buildspec.json"
$InstallerScript = Join-Path $ProjectRoot "installer\obs-voice-isolator.iss"
$PluginDll = Join-Path $ProjectRoot "dist\obs-voice-isolator\bin\64bit\obs-voice-isolator.dll"
$ReleaseDirectory = Join-Path $ProjectRoot "release"

if (-not (Test-Path $BuildSpecPath)) {
    throw "No se encontró buildspec.json."
}

if (-not (Test-Path $InstallerScript)) {
    throw "No se encontró el archivo del instalador: $InstallerScript"
}

if (-not (Test-Path $PluginDll)) {
    throw "No se encontró la DLL compilada. Ejecuta primero .\scripts\build-windows.ps1"
}

$BuildSpec = Get-Content $BuildSpecPath -Raw | ConvertFrom-Json
$Version = $BuildSpec.version

$RegistryPaths = @(
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
)

$InnoSetup = Get-ItemProperty $RegistryPaths -ErrorAction SilentlyContinue |
    Where-Object {
        $_.DisplayName -like "Inno Setup*"
    } |
    Select-Object -First 1

$Compiler = $null

if ($InnoSetup.InstallLocation) {
    $Candidate = Join-Path $InnoSetup.InstallLocation "ISCC.exe"

    if (Test-Path $Candidate) {
        $Compiler = $Candidate
    }
}

if (-not $Compiler) {
    $Compiler = Get-ChildItem `
        "$env:LOCALAPPDATA\Programs",
        "$env:ProgramFiles",
        "${env:ProgramFiles(x86)}" `
        -Filter ISCC.exe `
        -Recurse `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $Compiler -or -not (Test-Path $Compiler)) {
    throw "No fue posible encontrar ISCC.exe."
}

New-Item -ItemType Directory -Force $ReleaseDirectory | Out-Null

$ExpectedInstaller = Join-Path `
    $ReleaseDirectory `
    "OBS-Voice-Isolator-Setup-$Version.exe"

Remove-Item -Force $ExpectedInstaller -ErrorAction SilentlyContinue

Write-Host "Compilando instalador con:"
Write-Host "  $Compiler"
Write-Host ""
Write-Host "Versión:"
Write-Host "  $Version"

& $Compiler `
    "/DMyAppVersion=$Version" `
    $InstallerScript

if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup falló con código $LASTEXITCODE."
}

if (-not (Test-Path $ExpectedInstaller)) {
    throw "Inno Setup terminó, pero no se encontró: $ExpectedInstaller"
}

Write-Host ""
Write-Host "Instalador generado correctamente:"
Write-Host "  $ExpectedInstaller"
