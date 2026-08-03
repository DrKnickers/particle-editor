@echo off
REM Build the reference-object bounds probe as a standalone x64 console exe.
REM Drives the editor's real ReferenceObjectMesh::Load (no D3D device needed).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\dump_refbounds.exe /Fo:tests\obj\ ^
   tests\dump_refbounds.cpp ^
   src\AloModel.cpp src\ReferenceObjectMesh.cpp src\ChunkReader.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
