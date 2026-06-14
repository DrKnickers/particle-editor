@echo off
REM Build the GameObjectCatalog reader unit test as a standalone x64
REM console exe. Links the catalog + XML reader (xml.cpp + static expat) +
REM the AloModel decoder (for ProbeModelSkinned) + files + utils data-model
REM TUs; no engine / D3D-coupled TUs (managers.h is included only for the
REM abstract IFileManager interface).
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DXML_STATIC /DXML_UNICODE_WCHAR_T /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_game_object_catalog.exe /Fo:tests\obj\ ^
   tests\test_game_object_catalog.cpp ^
   src\GameObjectCatalog.cpp src\AloModel.cpp src\ChunkReader.cpp ^
   src\xml.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" /LIBPATH:"libs\expat-2.2.0\x64\Debug" ^
   expatw_static.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
