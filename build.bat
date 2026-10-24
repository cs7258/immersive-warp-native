@echo off
REM ============================================================
REM  Immersive Warp Native - build script (run on Windows x64)
REM  1st run: install MSVC Build Tools + Windows SDK if missing
REM ============================================================

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% goto NOTOOLS

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSROOT=%%i
if "%VSROOT%"=="" goto NOTOOLS

echo Using Visual Studio at %VSROOT%
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto FAIL

if not exist build mkdir build
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /DUNICODE /D_UNICODE ^
  src\Config.cpp src\Engine.cpp src\Windows.cpp src\CmdNet.cpp src\Mqtt.cpp src\Inject.cpp src\MagEngine.cpp src\HookIPC.cpp src\main.cpp ^
  /Fe:build\ImmersiveWarpNative.exe /Fo:build\ ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib comdlg32.lib windowscodecs.lib ws2_32.lib advapi32.lib
if errorlevel 1 goto FAIL

REM ---- scanout pipeline Phase 1: hook DLL + injector ----
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /DUNICODE /D_UNICODE /LD ^
  src\hook\WarpHook.cpp ^
  /Fe:build\WarpHook.dll /Fo:build\ ^
  /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d3dcompiler.lib kernel32.lib user32.lib
if errorlevel 1 goto FAIL

cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /DUNICODE /D_UNICODE ^
  src\hook\Injector.cpp ^
  /Fe:build\IWNInject.exe /Fo:build\ ^
  /link /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup kernel32.lib user32.lib
if errorlevel 1 goto FAIL

REM ---- external D3D11 injection target for regression (injecttest) ----
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /DUNICODE /D_UNICODE ^
  src\probe\D3DProbe.cpp ^
  /Fe:build\IWND3DProbe.exe /Fo:build\ ^
  /link /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup d3d11.lib dxgi.lib user32.lib
if errorlevel 1 goto FAIL

echo.
echo BUILD OK: build\ImmersiveWarpNative.exe build\WarpHook.dll build\IWNInject.exe build\IWND3DProbe.exe
exit /b 0

:NOTOOLS
echo MSVC Build Tools not found. Install with (one line, admin shell):
echo.
echo   winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo.
echo Then re-run this script.
exit /b 1

:FAIL
echo BUILD FAILED - see errors above.
exit /b 1
