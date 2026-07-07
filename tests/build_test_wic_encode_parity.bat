@echo off
REM Build the WIC-vs-GDI+ PNG encode parity test ([R3b]).
REM Links the two production encode TUs directly (WicEncode.cpp +
REM WindowCapture.cpp). No DXSDK include: these TUs use only Win10-SDK
REM headers (the app's per-file include-order override exists for the same
REM reason -- see WicEncode.cpp's vcxproj entry).
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_wic_encode_parity.exe /Fo:tests\obj\ ^
   tests\test_wic_encode_parity.cpp ^
   src\host\WicEncode.cpp src\host\WindowCapture.cpp ^
   /link gdiplus.lib windowscodecs.lib ole32.lib oleaut32.lib user32.lib gdi32.lib shlwapi.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
