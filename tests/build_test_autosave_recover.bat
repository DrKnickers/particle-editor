@echo off
REM Build the production-linked autosave filesystem/recovery contract test.
REM Links Autosave.cpp plus the exact ParticleSystem serializer/parser TUs used
REM by the editor. AUTOSAVE_TESTING exposes only the pre-commit corruption hook;
REM directory selection still flows through production GetTempPathW and
REM ScanForOrphan.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DAUTOSAVE_TESTING ^
   /DXML_STATIC /DXML_UNICODE_WCHAR_T /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_autosave_recover.exe /Fo:tests\obj\ ^
   tests\test_autosave_recover.cpp src\Autosave.cpp ^
   src\ParticleSystem.cpp src\ChunkReader.cpp src\ChunkWriter.cpp ^
   src\crc32.cpp src\files.cpp src\utils.cpp src\LinkGroup.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib ^
   shlwapi.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
