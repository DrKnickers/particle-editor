@echo off
REM Build the ParticleSystem .alo load-hardening + save-fidelity regression test
REM (audit C1 round-trip + A2/A3/A4 + string-terminator corpus) as a standalone
REM x64 console exe. Links the exact data-model TU set ParticleSystem.cpp needs
REM (same as test_emitter_reorder): ParticleSystem + ChunkReader + ChunkWriter +
REM crc32 + files + utils + LinkGroup. ParticleSystem.cpp transitively includes
REM xml.h, so the expat include path is needed to compile; no XML symbol is
REM exercised, so the expat lib is NOT linked. d3d9/d3dx9 symbols are referenced
REM (not exercised) by the data model.
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DXML_STATIC /DXML_UNICODE_WCHAR_T /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_alo_roundtrip.exe /Fo:tests\obj\ ^
   tests\test_alo_roundtrip.cpp ^
   src\ParticleSystem.cpp src\ChunkReader.cpp src\ChunkWriter.cpp ^
   src\crc32.cpp src\files.cpp src\utils.cpp src\LinkGroup.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
