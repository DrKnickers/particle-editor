@echo off
REM Build the blend-mode classifier unit test as a standalone x64 console exe.
REM Header-only classifier -> single TU, no engine object files / DX libs.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

if not exist tests\obj mkdir tests\obj

REM ParticleSystem.h transitively includes types.h -> d3dx9.h, so the DirectX SDK
REM include path is required to COMPILE (nothing is instantiated, so no DX libs
REM / engine TUs need to LINK).
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_blend_mode_classify.exe /Fo:tests\obj\ ^
   tests\test_blend_mode_classify.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
