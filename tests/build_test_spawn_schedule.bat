@echo off
REM Build the spawn-rate reconcile clamp regression test as a standalone
REM x64 console exe. SpawnSchedule.h is header-only (std only).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_spawn_schedule.exe /Fo:tests\obj\ ^
   tests\test_spawn_schedule.cpp ^
   /link kernel32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
