@echo off
REM Build the SkydomeEnvironment reader unit test as a standalone x64
REM console exe. Links the XML reader (xml.cpp + static expat) + files + utils
REM data-model TUs; no engine / D3D-coupled TUs (managers.h is included only for
REM the abstract IFileManager interface).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /DXML_STATIC /DXML_UNICODE_WCHAR_T /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_skydome_environment.exe /Fo:tests\obj\ ^
   tests\test_skydome_environment.cpp ^
   src\SkydomeEnvironment.cpp src\xml.cpp src\files.cpp src\utils.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" /LIBPATH:"libs\expat-2.2.0\x64\Debug" ^
   expatw_static.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
