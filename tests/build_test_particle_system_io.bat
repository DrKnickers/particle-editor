@echo off
REM Build the ParticleSystemIO disk-level contract test as a standalone x64
REM console exe. The wrapper bodies (LoadParticleSystem/SaveParticleSystem)
REM live in src\main.cpp inside the composed-app TU, so this test links the
REM exact machinery they wrap instead: PhysicalFile + ParticleSystem(IFile*)
REM + ParticleSystem::write, over the same data-model TU set as
REM test_alo_roundtrip (ParticleSystem + ParticleSystemSerialization + ChunkReader + ChunkWriter + crc32 +
REM files + utils + LinkGroup). ParticleSystem.cpp transitively includes
REM xml.h, so the expat include path is needed to compile; no XML symbol is
REM exercised, so the expat lib is NOT linked.
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
   /Fe:tests\test_particle_system_io.exe /Fo:tests\obj\ ^
   tests\test_particle_system_io.cpp ^
   src\ParticleSystem.cpp src\ParticleSystemSerialization.cpp src\ChunkReader.cpp src\ChunkWriter.cpp ^
   src\crc32.cpp src\files.cpp src\utils.cpp src\LinkGroup.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
