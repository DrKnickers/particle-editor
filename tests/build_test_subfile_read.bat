@echo off
REM Build the SubFile::read clamp regression test as a standalone x64 console exe.
REM Pure file-layer TUs (files + utils for LoadString in the exception ctors); no
REM engine / D3D. See tests/test_subfile_read.cpp.
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_subfile_read.exe /Fo:tests\obj\ ^
   tests\test_subfile_read.cpp ^
   src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
