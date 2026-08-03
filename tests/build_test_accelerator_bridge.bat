@echo off
REM Build the host::AcceleratorBridge unit test as a standalone x64 console
REM exe. Compiles the production TU (src\host\AcceleratorBridge.cpp) directly;
REM no engine / D3D-coupled TUs are needed. The DXSDK include path and
REM d3dx9.lib below are inherited from the shared bat template (see
REM build_test_webview_modal_policy.bat) rather than actual dependencies.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_accelerator_bridge.exe /Fo:tests\obj\ ^
   tests\test_accelerator_bridge.cpp ^
   src\host\AcceleratorBridge.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
