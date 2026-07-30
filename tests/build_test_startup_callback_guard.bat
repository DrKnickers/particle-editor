@echo off
REM Build the async-startup liveness regression test as a standalone x64
REM console exe, linking the exact production callback-adapter translation unit.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_startup_callback_guard.exe /Fo:tests\obj\ ^
   tests\test_startup_callback_guard.cpp src\host\StartupCallbackAdapter.cpp ^
   /link kernel32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
