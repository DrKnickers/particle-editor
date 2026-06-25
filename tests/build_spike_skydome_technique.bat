@echo off
REM Step-0] Build the skydome-technique-validation spike (standalone
REM D3D9Ex + D3DX exe). Diagnostic only; not a CI unit test.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" ^
   /Fe:tests\spike_skydome_technique.exe /Fo:tests\obj\ ^
   tests\spike_skydome_technique.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib user32.lib gdi32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
