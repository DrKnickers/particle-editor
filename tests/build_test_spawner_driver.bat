@echo off
REM Build the SpawnerDriver clamp/schedule unit test as a standalone x64
REM console exe. Compiles the production TU (src\SpawnerDriver.cpp) directly;
REM the two non-virtual Engine members it calls (SpawnParticleSystem /
REM ActiveSpawnerInstanceCount) are defined as recording stubs INSIDE the test
REM TU, so engine.cpp and the rest of the D3D-coupled stack are neither
REM compiled nor linked. engine.h and its header-only deps come from src.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_spawner_driver.exe /Fo:tests\obj\ ^
   tests\test_spawner_driver.cpp ^
   src\SpawnerDriver.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
