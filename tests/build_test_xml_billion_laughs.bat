@echo off
REM Build the XML entity-expansion (billion-laughs) DoS-guard regression test
REM (audit F-XML) as a standalone x64 console exe. Links the XML reader + static
REM expat + files + utils. The prebuilt expatw_static.lib is a RELEASE (/MD)
REM build, so this TU is compiled /MD (release CRT) to match it and avoid a
REM CRT-mismatch link error; the other tests' /MDd is not required here.
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MD /Zi ^
   /DUNICODE /D_UNICODE /D_WINDOWS /DXML_STATIC /DXML_UNICODE_WCHAR_T /D_CRT_SECURE_NO_WARNINGS ^
   /I "libs\expat-2.2.0\include" /I "%DXSDK_DIR%Include" /I "src" ^
   /I "packages\Microsoft.Web.WebView2.1.0.3967.48\build\native\include" ^
   /Fe:tests\test_xml_billion_laughs.exe /Fo:tests\obj\ ^
   tests\test_xml_billion_laughs.cpp ^
   src\xml.cpp src\files.cpp src\utils.cpp ^
   /link /NODEFAULTLIB:LIBCMT /LIBPATH:"%DXSDK_DIR%Lib\x64" /LIBPATH:"libs\expat-2.2.0\x64\Release" ^
   expatw_static.lib d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
