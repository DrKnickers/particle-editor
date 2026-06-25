@echo off
REM Build the asset-name path-safety regression test (audit F-PATH) as a
REM standalone x64 console exe. AssetPathSafety.h is header-only (no .cpp);
REM this TU pulls in nothing else, so no src/*.cpp are linked. The DirectX
REM libs are linked only to match the other tests' floor (unreferenced here).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_asset_path_safety.exe /Fo:tests\obj\ ^
   tests\test_asset_path_safety.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
