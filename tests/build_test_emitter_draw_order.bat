@echo off
REM Build the emitter draw-order unit test as a standalone x64 console exe.
REM Header-only decision (src/EmitterDrawOrder.h, std-only) -> single TU, no
REM engine object files, no DirectX SDK include, no DX libs to link.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

if not exist tests\obj mkdir tests\obj

REM No /Zi: a passing unit test needs no PDB, and a repo-root vc<ver>.pdb would
REM collide with a concurrent MSBuild (C1041). /Fd isolates any incidental PDB.
cl /nologo /EHsc /std:c++17 /MDd ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_emitter_draw_order.exe /Fo:tests\obj\ /Fd:tests\obj\test_emitter_draw_order.pdb ^
   tests\test_emitter_draw_order.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
