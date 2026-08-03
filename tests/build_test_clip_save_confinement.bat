@echo off
REM Build the SavePathConfine.h record save-path confinement unit test as a
REM standalone x64 console exe. SavePathConfine.h is engine-free and depends
REM only on Win32 path APIs; the test links shell32 for temp tree creation and
REM cleanup helpers.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "src" ^
   /Fe:tests\test_clip_save_confinement.exe /Fo:tests\obj\ ^
   tests\test_clip_save_confinement.cpp ^
   /link shell32.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
