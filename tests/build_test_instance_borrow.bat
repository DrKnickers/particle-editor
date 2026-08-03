@echo off
REM Build the device-free tokenized instance-borrow registry + binding oracle.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi /I "src" ^
   /Fe:tests\test_instance_borrow.exe /Fo:tests\obj\ ^
   tests\test_instance_borrow.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
