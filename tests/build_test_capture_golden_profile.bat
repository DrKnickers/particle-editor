@echo off
REM Build the render-golden capture policy and production-wiring regression
REM test as a standalone x64 console exe. The policy is header-only; the test
REM reads the three production call-site TUs so no D3D/WebView TU is linked.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_capture_golden_profile.exe /Fo:tests\obj\ ^
   tests\test_capture_golden_profile.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
