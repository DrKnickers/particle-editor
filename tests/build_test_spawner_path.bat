@echo off
REM Build the spawner shaped-path test (EvalSpawnerPath) as a
REM standalone x64 console exe. SpawnerPath.h is header-only + pure, so
REM this links nothing from the engine — only the DirectX libs that
REM provide the D3DXVECTOR3 helpers.
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_spawner_path.exe /Fo:tests\obj\ ^
   tests\test_spawner_path.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
