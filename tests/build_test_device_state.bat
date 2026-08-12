@echo off
REM Build the D3D9Ex device-state classification test (2026-07 audit).
REM src\DeviceState.h is a pure header — it needs d3d9.h for the HRESULT
REM constants and nothing else, so no engine/D3D TU is compiled or linked.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed ^& exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_device_state.exe /Fo:tests\obj\ tests\test_device_state.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
