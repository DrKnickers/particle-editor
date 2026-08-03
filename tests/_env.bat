@echo off
REM ---------------------------------------------------------------------------
REM Shared toolchain setup for the tests\build_*.bat host-free unit builders.
REM Discovers the toolchain AT RUNTIME so no machine-specific path is baked in:
REM   * Visual Studio C++ (vcvars64) via vswhere.exe -- the fixed-location locator
REM     Microsoft ships with every VS 2017+ install (any edition / install path).
REM   * the legacy DirectX SDK (June 2010) via the DXSDK_DIR env var its installer
REM     sets (falls back to the conventional Program Files location).
REM Exports DXSDK_INC / DXSDK_LIB for callers that compile against d3dx9.
REM Usage from a build script:  call "%~dp0_env.bat" || exit /b 1
REM ---------------------------------------------------------------------------

REM --- Visual Studio (skip if a developer environment is already active) ------
if defined VCToolsInstallDir goto :dxsdk

set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_VSWHERE%" goto :no_vswhere

set "_VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "_VSINSTALL=%%i"
if not defined _VSINSTALL goto :no_vs
REM 2>nul: vcvars/vsdevcmd emit a benign internal "vswhere not recognized" to
REM stderr; we gate on its exit code instead, so silence the noise.
call "%_VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 goto :no_vcvars
REM Post-condition: vcvars can exit 0 yet not populate the env on a damaged install.
if not defined VCToolsInstallDir goto :no_vcvars

:dxsdk
REM Set DXSDK_DIR if unset so scripts that reference %DXSDK_DIR% raw also work; then
REM derive INC/LIB and VALIDATE (a set-but-stale DXSDK_DIR must fail loudly here, not
REM cryptically at compile). DXSDK_DIR has a trailing backslash by installer convention.
if not defined DXSDK_DIR set "DXSDK_DIR=%ProgramFiles(x86)%\Microsoft DirectX SDK (June 2010)\"
set "DXSDK_INC=%DXSDK_DIR%Include"
set "DXSDK_LIB=%DXSDK_DIR%Lib\x64"
if not exist "%DXSDK_INC%\d3dx9.h" goto :no_dxsdk
goto :done

:no_vswhere
echo [_env] vswhere.exe not found; install Visual Studio with the "Desktop development with C++" workload. 1>&2
exit /b 1
:no_vs
echo [_env] No Visual Studio install with the C++ toolset (VC.Tools.x86.x64) was found. 1>&2
exit /b 1
:no_vcvars
echo [_env] vcvars64.bat failed to initialize the x64 build environment. 1>&2
exit /b 1
:no_dxsdk
echo [_env] DirectX SDK (June 2010) not found (no d3dx9.h under "%DXSDK_INC%"). 1>&2
echo [_env] Install it or set DXSDK_DIR to a valid SDK root. 1>&2
exit /b 1

:done
exit /b 0
