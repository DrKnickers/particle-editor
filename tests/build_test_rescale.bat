@echo off
REM Build the DoRescaleEmitter unit test (src/Rescale.cpp) as a standalone x64
REM console exe. Links Rescale.cpp plus the exact data-model TU set
REM ParticleSystem.cpp + ParticleSystemSerialization.cpp need (same as
REM test_alo_roundtrip): ParticleSystem + ParticleSystemSerialization +
REM ChunkReader + ChunkWriter + crc32 + files + utils + LinkGroup.
REM ParticleSystem.cpp transitively includes xml.h, so the expat include path is
REM needed to compile; no XML symbol is exercised, so the expat lib is NOT
REM linked. d3d9/d3dx9 symbols are referenced (not exercised) by the data model.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DXML_STATIC /DXML_UNICODE_WCHAR_T /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_rescale.exe /Fo:tests\obj\ ^
   tests\test_rescale.cpp ^
   src\Rescale.cpp src\ParticleSystem.cpp src\ParticleSystemSerialization.cpp src\ChunkReader.cpp src\ChunkWriter.cpp ^
   src\crc32.cpp src\files.cpp src\utils.cpp src\LinkGroup.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
