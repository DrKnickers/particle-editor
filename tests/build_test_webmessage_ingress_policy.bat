@echo off
REM Build the bridge-ingress size-cap policy test as a standalone x64 console
REM exe. WebMessageIngressPolicy.h is a pure header with no WebView2 or D3D9
REM dependency (that is the point of the extraction), so no extra TUs link --
REM same shape as build_test_webview_modal_policy.bat.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_webmessage_ingress_policy.exe /Fo:tests\obj\ ^
   tests\test_webmessage_ingress_policy.cpp

set ERR=%errorlevel%
popd
exit /b %ERR%
