# Build-driver.ps1 - compile IWNPPL.sys in CI using the WDK NuGet
# package headers/libs plus the runner's MSVC toolchain.
# Assumes VsDevCmd.bat (x64) has already been called in this session.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$pkg = Join-Path $root "..\wdkpkg"
if (-not (Test-Path $pkg)) { throw "wdkpkg not found: $pkg" }

$ntddk = Get-ChildItem -Path $pkg -Recurse -Filter ntddk.h | Select-Object -First 1
if (-not $ntddk) { throw "ntddk.h not found in WDK package" }
$kmInc    = $ntddk.DirectoryName
$incRoot  = Split-Path $kmInc -Parent
$incShared = Join-Path $incRoot "shared"
$incCrt   = Join-Path $kmInc "crt"
Write-Output ("km include : " + $kmInc)

$libDir = Get-ChildItem -Path $pkg -Recurse -Filter ntoskrnl.lib |
    Where-Object { $_.FullName -match '\\km\\x64' } | Select-Object -First 1
if (-not $libDir) { throw "ntoskrnl.lib (km/x64) not found" }
$libPath = $libDir.DirectoryName
Write-Output ("km lib     : " + $libPath)

New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null

& cl /nologo /kernel /W4 /O2 /GS- /D_AMD64_ /DAMD64 /D_NTSYSTEM /D_WIN64 `
    /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=NTDDI_WIN10_19H1 `
    /I"$kmInc" /I"$incShared" /I"$incCrt" `
    /c "$root\IWNPPL.c" /Fo"$root\build\IWNPPL.obj"
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

& link /nologo /OUT:"$root\build\IWNPPL.sys" /MACHINE:X64 `
    /SUBSYSTEM:NATIVE,10.00 /DRIVER /ENTRY:DriverEntry /NODEFAULTLIB `
    /INCREMENTAL:NO /INTEGRITYCHECK /PDB:"$root\build\IWNPPL.pdb" `
    "$root\build\IWNPPL.obj" `
    (Join-Path $libPath "ntoskrnl.lib") `
    (Join-Path $libPath "hal.lib")
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Output ("built: " + (Get-Item (Join-Path $root "build\IWNPPL.sys")).Length + " bytes")
