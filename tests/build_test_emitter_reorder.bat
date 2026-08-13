@echo off
REM Build the emitter-reorder regression test (audit fix C) as a standalone
REM x64 console exe. Links only the data-model TUs ParticleSystem.cpp + ParticleSystemSerialization.cpp need,
REM against the DirectX SDK libs (symbols referenced but not exercised).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DXML_STATIC /D_CRT_SECURE_NO_WARNINGS /DXML_UNICODE_WCHAR_T ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_emitter_reorder.exe /Fo:tests\obj\ ^
   tests\test_emitter_reorder.cpp ^
   src\ParticleSystem.cpp src\ParticleSystemSerialization.cpp src\ChunkReader.cpp src\ChunkWriter.cpp ^
   src\crc32.cpp src\files.cpp src\utils.cpp src\LinkGroup.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
