@echo off
REM Build the SceneOverscan.h pure-helper unit test as a standalone x64 console
REM exe. SceneOverscan.h is header-only with no engine/Win32 deps, so this links
REM only the test TU.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "src" ^
   /Fe:tests\test_scene_overscan.exe /Fo:tests\obj\ ^
   tests\test_scene_overscan.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
