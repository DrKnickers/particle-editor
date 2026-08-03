@echo off
REM Build the RecordOutputSafety.h pure-helper unit test as a standalone x64
REM console exe. RecordOutputSafety.h is header-only with no engine/Win32 deps
REM (it takes an already-enumerated listing), so this links only the test TU.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "src" /I "src\host" ^
   /Fe:tests\test_record_output_safety.exe /Fo:tests\obj\ ^
   tests\test_record_output_safety.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
