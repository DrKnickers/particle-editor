@echo off
REM Build the host::CompositeUiOverEngine unit test (src/host/HeadlessComposite.h).
REM Pure pixel math — header-only, no host/WebView2/D3D9, no src TUs or libs.
setlocal
call "%~dp0_env.bat" || exit /b 1
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi /I src\host ^
   /Fe:tests\test_headless_composite.exe /Fo:tests\obj\ ^
   tests\test_headless_composite.cpp
