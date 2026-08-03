@echo off
REM Build the AloModel malformed-input fuzz test as a standalone x64 console
REM exe. Data-model TUs only (AloModel + ChunkReader + files + utils), the same
REM set as build_test_alo_model.bat minus ReferenceObjectMesh (no render-core
REM consumer is exercised). utils.cpp provides LoadString used by the exception
REM ctors.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_alo_fuzz.exe /Fo:tests\obj\ ^
   tests\test_alo_fuzz.cpp ^
   src\AloModel.cpp src\ChunkReader.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
