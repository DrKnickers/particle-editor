@echo off
REM Build the BridgeDispatcher spawner-state production-oracle test. The
REM tested mutations/resolver are header-visible, so this links only the real
REM SpawnerDriver implementation and the two Engine link stubs in the test.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" /I "src\host" ^
   /Fe:tests\test_bridge_spawner_state.exe /Fo:tests\obj\ ^
   tests\test_bridge_spawner_state.cpp ^
   src\SpawnerDriver.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
