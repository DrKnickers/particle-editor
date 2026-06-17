@echo off
REM Throwaway: build the .alo bbox measurement (dev-box only, not CI).
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\measure_alo_bbox.exe /Fo:tests\obj\ ^
   tests\measure_alo_bbox.cpp ^
   src\AloModel.cpp src\ChunkReader.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
