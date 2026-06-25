@echo off
REM Build the MegaFile header-validation fuzz test (audit G9) as a standalone
REM x64 console exe. MegaFiles.cpp #includes xml.h (-> expat headers + managers.h)
REM so the expat include path is needed to COMPILE, but the MegaFile ctor never
REM exercises any XML symbol, so xml.cpp / the expat lib are NOT needed to LINK.
REM Data-model TUs only: MegaFiles + files + utils + crc32.
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
   /Fe:tests\test_meg_fuzz.exe /Fo:tests\obj\ ^
   tests\test_meg_fuzz.cpp ^
   src\MegaFiles.cpp src\files.cpp src\utils.cpp src\crc32.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
