@echo off
REM Build the AloModel decoder unit test as a standalone x64 console exe.
REM Data-model TUs only (AloModel + ChunkReader + files + utils); no engine /
REM D3D-coupled TUs. utils.cpp provides LoadString used by the exception ctors.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /DALO_MODEL_TEST_PROBES ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_alo_model.exe /Fo:tests\obj\ ^
   tests\test_alo_model.cpp ^
   src\AloModel.cpp src\ReferenceObjectMesh.cpp src\ChunkReader.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
