@echo off
REM Build the PhysicalFile write-contract + handle-lifetime regression test as a
REM standalone x64 console exe. Unlike the header-only tests this one links
REM src\files.cpp (PhysicalFile lives there), plus src\utils.cpp for the app's
REM LoadStringW resource-string helper that files.cpp calls when it throws.
REM The test exe carries no string table, so that helper just yields an empty
REM message — the tests assert on control flow and the filesystem, not text.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_physicalfile_write.exe /Fo:tests\obj\ ^
   tests\test_physicalfile_write.cpp ^
   src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
