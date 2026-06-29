@echo off
REM Build the device-free asset path safety integration test as a standalone
REM x64 console exe. Mirrors build_test_game_object_catalog.bat, adding the
REM skydome environment TU.
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
   /Fe:tests\test_asset_safety_integration.exe /Fo:tests\obj\ ^
   tests\test_asset_safety_integration.cpp ^
   src\GameObjectCatalog.cpp src\SkydomeEnvironment.cpp src\AloModel.cpp src\ChunkReader.cpp ^
   src\xml.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" /LIBPATH:"libs\expat-2.2.0\x64\Debug" ^
   expatw_static.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
