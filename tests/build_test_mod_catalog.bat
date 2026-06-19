@echo off
REM Build the ModScan.h Win32 directory-scan unit test as a standalone x64
REM console exe. ModScan.h is engine-free (only <windows.h> + the pure ModLayers.h),
REM so this links only the test TU plus the Win32 shell libs SHCreateDirectoryExW /
REM SHFileOperation / PathIsDirectory need.
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "src" ^
   /Fe:tests\test_mod_catalog.exe /Fo:tests\obj\ ^
   tests\test_mod_catalog.cpp ^
   /link shell32.lib shlwapi.lib ole32.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
