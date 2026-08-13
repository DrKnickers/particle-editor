@echo off
REM Build the engine-free persisted-settings registry round-trip test. The
REM production reader links only registry helpers and deliberately pulls no
REM Engine, D3D9, or WebView2 implementation.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" /I "src\host" ^
   /Fe:tests\test_settings_restore.exe /Fo:tests\obj\ ^
   tests\test_settings_restore.cpp ^
   src\host\RestoredSettings.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
