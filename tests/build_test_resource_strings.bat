@echo off
REM Build the resource-string migration gate as a standalone x64 console
REM exe. It loads the built ParticleEditor.exe as a data module and asserts the
REM migrated IDS_* STRINGTABLE entries resolve (and the kept RCDATA/icon/bitmap
REM resources survived the .rc trim). Links user32 for LoadStringW/FindResourceW.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MD /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "src" ^
   /Fe:tests\test_resource_strings.exe /Fo:tests\obj\ ^
   tests\test_resource_strings.cpp ^
   /link user32.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
