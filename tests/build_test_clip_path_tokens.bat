@echo off
REM Build the ${TOKEN} path-expander unit test (#494 follow-up) as a standalone
REM x64 console exe. Pure header under test (src/host/ClipPathTokens.h) -> single
REM TU, no DX / json / engine object files needed.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I "src\host" ^
   /Fe:tests\test_clip_path_tokens.exe /Fo:tests\obj\ ^
   tests\test_clip_path_tokens.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
