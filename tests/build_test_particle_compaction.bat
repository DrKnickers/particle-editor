@echo off
REM Build the particle-compaction regression test as a standalone
REM x64 console exe. The algorithm is header-only pure vector/set logic.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DXML_STATIC /D_CRT_SECURE_NO_WARNINGS /DXML_UNICODE_WCHAR_T ^
   /I "src" ^
   /Fe:tests\test_particle_compaction.exe /Fo:tests\obj\ ^
   tests\test_particle_compaction.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
