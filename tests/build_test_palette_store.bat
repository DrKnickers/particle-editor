@echo off
REM Build the PaletteStore data-layer regression test as a standalone x64 console exe.
REM Links the exact data-model TU set documented in test_palette_store.cpp; no GUI / D3D-coupled TUs.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_palette_store.exe /Fo:tests\obj\ ^
   tests\test_palette_store.cpp ^
   src\UI\PaletteStore.cpp src\crc32.cpp src\utils.cpp ^
   /link Shell32.lib User32.lib Advapi32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
