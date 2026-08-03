@echo off
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Fe:tests\test_ref_lock.exe /Fo:tests\obj\ tests\test_ref_lock.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
