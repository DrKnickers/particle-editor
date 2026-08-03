@echo off
REM Build the muzzle-flash filter unit test (IsMuzzleFlashMesh) as a standalone
REM x64 console exe. Pure-header test (string only); links nothing -- the
REM test_reference_world precedent.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Fe:tests\test_muzzle_flash.exe /Fo:tests\obj\ tests\test_muzzle_flash.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
