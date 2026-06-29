@echo off
REM Build the Autosave.h pure recovery-outcome helper unit test as a standalone x64
REM console exe. We compile only this TU; the non-inline Autosave functions are
REM declarations we never call, so no link against Autosave.cpp is needed.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "src" ^
   /Fe:tests\test_autosave_recover.exe /Fo:tests\obj\ ^
   tests\test_autosave_recover.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
