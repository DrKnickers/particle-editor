@echo off
REM Build the WM_CLOSE veto-decision regression test as a standalone
REM x64 console exe. CloseDecision.h is header-only (pure function, no .cpp);
REM no src/*.cpp are linked. The DirectX libs are linked only to match the
REM other tests' floor (unreferenced here).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_close_guard.exe /Fo:tests\obj\ ^
   tests\test_close_guard.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
