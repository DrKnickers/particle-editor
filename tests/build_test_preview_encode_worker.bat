@echo off
REM Build the PreviewEncodeWorker queue-contract test as a standalone x64
REM console exe. PreviewEncodeWorker.h is header-only, but its WorkerLoop calls
REM TexturePalette::EncodePackedBgraToPngBytes, whose body lives in
REM src\UI\PaletteThumbs.cpp -- that TU pulls d3d9/d3dx9 headers for the decode
REM half it also carries, so the DX include/lib paths are needed even though no
REM device is ever created here. utils/files come along as PaletteThumbs' own
REM dependencies; AssetPathSafety and managers are header-only.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" /I "src\host" ^
   /Fe:tests\test_preview_encode_worker.exe /Fo:tests\obj\ ^
   tests\test_preview_encode_worker.cpp ^
   src\UI\PaletteThumbs.cpp src\utils.cpp src\files.cpp src\host\PerfTrace.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3d9.lib d3dx9.lib gdiplus.lib shlwapi.lib ^
   ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
