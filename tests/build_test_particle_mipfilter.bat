@echo off
REM Build the ALO_PARTICLE_MIPFILTER parser unit test (#481) as a standalone
REM x64 console exe. Pure header under test (src/ParticleMipFilter.h) -> single
REM TU, no DX includes or engine object files needed.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_particle_mipfilter.exe /Fo:tests\obj\ ^
   tests\test_particle_mipfilter.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
